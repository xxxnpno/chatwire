#pragma once

// chatwire.ws.server — the local WebSocket server.
//
// ===========================================================================
// SHAPE
// ===========================================================================
// One accept thread, one thread per client.  Not because that scales to
// thousands of connections — it does not — but because the real client count
// here is "a few tools on the same machine", and a thread-per-client server is
// small enough to audit for the leaks and races that actually matter when you
// are living inside someone's game process.
//
// ===========================================================================
// BOUND TO LOOPBACK, DELIBERATELY
// ===========================================================================
// The listener binds 127.0.0.1, never 0.0.0.0.  This socket can send chat as
// the player and read everything they see; exposing it to the network would
// hand that to anyone who can reach the machine.  Loopback-only keeps it to
// software the user already ran.  There is no authentication, and that is only
// defensible BECAUSE of the bind address — so the bind address is not a
// configuration knob.
//
// ===========================================================================
// LIFETIME, WHICH IS THE WHOLE DIFFICULTY
// ===========================================================================
// The failure mode for an injected DLL is a thread that outlives the code it is
// running.  Every rule here exists for that:
//
//   * every thread has a way OUT that does not involve another thread closing a
//     descriptor it is blocked on.  The accept thread polls with a timeout and
//     re-reads the running flag; the client readers are woken by shutdown(),
//     which leaves the descriptor valid and owned by its reader.  See stop();
//   * a client is registered the moment it is accepted, BEFORE its handshake, so
//     that stop() can reach it.  One that was still reading its HTTP request
//     used to be on no list at all, and its thread was joined with nothing left
//     to wake it;
//   * it JOINS every thread before returning, so no thread can still be inside
//     this module when the library unloads;
//   * client threads never touch the client list except through the mutex, and
//     they remove themselves before exiting;
//   * a client that stops reading cannot stall the game: writes are best-effort
//     and a failed write closes that client rather than blocking the broadcaster.
//
// Sockets themselves are Winsock on Windows and BSD everywhere else; every
// difference between them is resolved in chatwire/net.hpp and none of it leaks
// into this file.
#include "chatwire/common.hpp"

#include "chatwire/log.hpp"
#include "chatwire/net.hpp"
#include "chatwire/ws/websocket.hpp"
namespace chatwire::ws::detail
{
    /*
        @brief One connected client, owning its socket.
        @details
        Held by shared_ptr so a broadcast can hold a client alive for the length
        of a send even if its own thread is concurrently removing it from the
        list.  That race is the classic thread-per-client use-after-free.
    */
    struct client
    {
        chatwire::net::socket_t sock{ chatwire::net::invalid_socket };
        /*
            A number that identifies this connection, and only this one.
            Monotonic and never reused within a run, so a feature can hold onto
            one after the client is gone and be told "no such client" rather
            than reaching whoever inherited the slot.  That is the property the
            `commands` feature needs: a registered command outlives the socket
            write that delivered its last event, and delivering somebody else's
            command to a new client would be worse than dropping it.
        */
        std::uint64_t id{ 0 };
        std::mutex write_mutex{};              // serialises frames on this socket
        std::atomic<bool> alive{ true };

        ~client()
        {
            chatwire::net::close_socket(sock);
        }

        client() = default;
        client(const client&)                    = delete;
        auto operator=(const client&) -> client& = delete;

        /*
            @brief Writes one frame.  Never blocks the caller indefinitely.
            @return false when the peer is gone; the caller drops the client.
        */
        auto send_frame(const ws::opcode op, const std::string_view payload) noexcept -> bool
        {
            if (!alive.load(std::memory_order_acquire)) { return false; }
            try
            {
                const auto frame{ ws::encode(op, payload) };
                const std::lock_guard<std::mutex> guard{ write_mutex };
                std::size_t sent{ 0 };
                while (sent < frame.size())
                {
                    const std::ptrdiff_t n{ chatwire::net::send_some(
                        sock, frame.data() + sent, frame.size() - sent) };
                    if (n <= 0)
                    {
                        // Retryable means a signal landed mid-write, not that the
                        // peer is gone -- and a JVM raises plenty of its own.
                        // Dropping the client here would disconnect somebody every
                        // time the collector ran.
                        if (n < 0 && chatwire::net::retryable(chatwire::net::last_error()))
                        {
                            continue;
                        }
                        alive.store(false, std::memory_order_release);
                        return false;
                    }
                    sent += static_cast<std::size_t>(n);
                }
                return true;
            }
            catch (...)
            {
                alive.store(false, std::memory_order_release);
                return false;
            }
        }
    };

    using client_ptr = std::shared_ptr<client>;
}

namespace chatwire::ws
{
    /*
        @brief What the server does with a decoded client message.
        @details
        A plain function pointer rather than std::function: the handler is
        installed once at startup and never changes, and a function pointer
        cannot throw on copy or dangle after a lambda's captures die.

        `client` is the connection the message came in on.  Most commands do not
        care -- the reply goes back down the same socket either way -- but one
        that registers something on the caller's behalf has to know WHOSE
        behalf, and a handler with no way to ask would have to invent a
        correlation scheme of its own.
    */
    using message_handler = std::string (*)(std::uint64_t client,
                                            std::string_view request) noexcept;

    /*
        @brief Called when a client connects or disconnects, with the new total.
        @details
        Runs on that client's own thread, so it must not block for long.
        chatwire uses it to announce the connection in the player's chat, which
        it does the same way everything else reaches the game: by calling it
        from the thread it is already on.

        The disconnect side is also where anything registered in that client's
        name is dropped, which is why the id is passed: a plugin that registered
        `/ping` and then died must not leave `/ping` swallowed forever with
        nobody left to answer it.
    */
    using presence_handler = void (*)(bool connected, std::uint64_t client,
                                      std::size_t total) noexcept;

    class server
    {
    public:
        server() = default;

        ~server() { this->stop(); }

        server(const server&)                    = delete;
        auto operator=(const server&) -> server& = delete;
        server(server&&)                         = delete;
        auto operator=(server&&) -> server&      = delete;

        /*
            @brief Binds 127.0.0.1:`port` and starts accepting.
            @return false if the port is taken or Winsock is unavailable; the
                    caller reports it and keeps running (chat hooks still work,
                    there is simply nothing to talk to).
        */
        [[nodiscard]] auto start(const std::uint16_t port, const message_handler handler,
                                 const presence_handler on_presence = nullptr) noexcept
            -> bool
        {
            if (this->running_.load(std::memory_order_acquire)) { return true; }
            this->handler_  = handler;
            this->presence_ = on_presence;

            if (!chatwire::net::startup())
            {
                chatwire::log::error("could not initialise the socket library");
                return false;
            }
            this->net_started_ = true;

            this->listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (this->listener_ == chatwire::net::invalid_socket)
            {
                chatwire::log::error("socket() failed: {}", chatwire::net::last_error());
                this->cleanup_net();
                return false;
            }

            // Without this, a restart inside the same game session hits
            // TIME_WAIT and fails to bind for ~2 minutes.
            chatwire::net::set_reuse_addr(this->listener_);
            chatwire::net::prepare(this->listener_);

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port   = chatwire::net::to_network_port(port);
            // Loopback ONLY.  See the file header.
            addr.sin_addr.s_addr = chatwire::net::loopback_address();

            if (::bind(this->listener_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
            {
                chatwire::log::error("bind(127.0.0.1:{}) failed: {}", port,
                                     chatwire::net::last_error());
                this->close_listener();
                this->cleanup_net();
                return false;
            }
            if (::listen(this->listener_, SOMAXCONN) != 0)
            {
                chatwire::log::error("listen() failed: {}", chatwire::net::last_error());
                this->close_listener();
                this->cleanup_net();
                return false;
            }

            this->running_.store(true, std::memory_order_release);
            try
            {
                this->accept_thread_ = std::thread{ [this] { this->accept_loop(); } };
            }
            catch (...)
            {
                chatwire::log::error("could not start the accept thread");
                this->running_.store(false, std::memory_order_release);
                this->close_listener();
                this->cleanup_net();
                return false;
            }

            // Report the port the OS ACTUALLY bound, not the one we asked for.
            // They differ whenever port 0 was requested (bind picks an ephemeral
            // one), and logging the request instead of the result is what turned
            // a one-line bug into a puzzle: the log said 0, the server was
            // listening on something else entirely, and no client could find it.
            this->bound_port_ = port;
            sockaddr_in            actual{};
            chatwire::net::addr_len actual_size{ sizeof(actual) };
            if (::getsockname(this->listener_, reinterpret_cast<sockaddr*>(&actual),
                              &actual_size) == 0)
            {
                this->bound_port_ = chatwire::net::from_network_port(actual.sin_port);
            }

            chatwire::log::info("websocket server listening on ws://127.0.0.1:{}",
                                this->bound_port_);
            return true;
        }

        /*
            @brief Stops accepting, drops every client, joins every thread.
            @details
            Idempotent, and safe to call from the destructor.  The ORDER is the
            point: nothing may join a thread that has not been given a way out,
            or join() hangs forever and the game freezes on unload.

            The two waking mechanisms are NOT the same, and assuming they were is
            what made the first version of this Windows-only:

              * the ACCEPT thread leaves on its own.  It polls the listener with
                a timeout and re-reads `running_` between polls, so clearing the
                flag is enough.  Closing the listener under it -- which is what
                this used to do, and which does wake accept() on Windows -- is on
                POSIX both a hang and a use-after-free: the descriptor number is
                handed back to the OS while a thread is still blocked on it, free
                to be reused by any other thread that opens a file.  So the
                listener is closed AFTER that thread is joined, never before.

              * the CLIENT readers are woken by shutdown(), which does return a
                blocked recv() on all three platforms.  shutdown() rather than
                close(): the reader still owns the descriptor and closes it on
                the way out, so again nobody frees a number somebody else holds.
        */
        auto stop() noexcept -> void
        {
            if (!this->running_.exchange(false, std::memory_order_acq_rel))
            {
                // Never started, or already stopped.  Still join in case start()
                // failed halfway.
                if (this->accept_thread_.joinable()) { this->accept_thread_.join(); }
                this->close_listener();
                return;
            }

            // `running_` is already false, so the accept thread exits at its next
            // poll.  Join it FIRST: after this returns, no new client can appear
            // and the listener is nobody's business but ours.
            if (this->accept_thread_.joinable()) { this->accept_thread_.join(); }
            this->close_listener();

            {
                const std::lock_guard<std::mutex> guard{ this->clients_mutex_ };
                for (auto& c : this->clients_)
                {
                    if (c)
                    {
                        c->alive.store(false, std::memory_order_release);
                        chatwire::net::shutdown_both(c->sock);
                    }
                }
            }

            {
                const std::lock_guard<std::mutex> guard{ this->threads_mutex_ };
                for (auto& t : this->client_threads_)
                {
                    if (t.joinable()) { t.join(); }
                }
                this->client_threads_.clear();
            }

            {
                const std::lock_guard<std::mutex> guard{ this->clients_mutex_ };
                this->clients_.clear();
            }

            this->cleanup_net();
            this->bound_port_ = 0u;
            chatwire::log::info("websocket server stopped");
        }

        /*
            @brief Sends `payload` to every connected client.
            @details
            Safe from any thread, including from inside a game detour — which is
            exactly where chat events come from, so this must never block on a
            slow client.  A client whose write fails is marked dead and reaped;
            the game thread is never held up by a peer that stopped reading.
        */
        auto broadcast(const std::string_view payload) noexcept -> void
        {
            std::vector<detail::client_ptr> snapshot;
            try
            {
                const std::lock_guard<std::mutex> guard{ this->clients_mutex_ };
                snapshot = this->clients_;      // shared_ptr copies keep them alive
            }
            catch (...)
            {
                return;
            }

            for (auto& c : snapshot)
            {
                if (c && !c->send_frame(opcode::text, payload))
                {
                    this->drop(c);
                }
            }
        }

        /*
            @brief Sends `payload` to ONE client, by id.
            @details
            Same rules as broadcast() -- safe from any thread, including from
            inside a game detour, and a failed write drops that client rather
            than blocking the caller.  It is the delivery half of the `commands`
            feature: a command belongs to the client that registered it, so its
            event goes there and nowhere else.

            Broadcasting instead would have been fewer lines and is the wrong
            shape twice over: every other connected tool would see a command it
            did not register, and two plugins registering the same name would
            both act on it.

            @return false when no such client is connected -- which is a normal
                    outcome rather than an error, since a plugin can disconnect
                    between the player typing and the event being delivered.
        */
        auto send_to(const std::uint64_t client, const std::string_view payload) noexcept
            -> bool
        {
            detail::client_ptr target;
            try
            {
                // The lookup holds the lock; the WRITE does not.  A send on a
                // peer that has stopped reading can block for as long as its
                // receive window stays full, and holding clients_mutex_ across
                // that would stall every other client's dispatch behind it --
                // and this is called from the game thread.
                const std::lock_guard<std::mutex> guard{ this->clients_mutex_ };
                for (const auto& c : this->clients_)
                {
                    if (c && c->id == client) { target = c; break; }
                }
            }
            catch (...) { return false; }

            if (!target) { return false; }
            if (target->send_frame(opcode::text, payload)) { return true; }

            this->drop(target);
            return false;
        }

        [[nodiscard]] auto client_count() const noexcept -> std::size_t
        {
            try
            {
                const std::lock_guard<std::mutex> guard{ this->clients_mutex_ };
                return this->clients_.size();
            }
            catch (...) { return 0u; }
        }

        [[nodiscard]] auto is_running() const noexcept -> bool
        {
            return this->running_.load(std::memory_order_acquire);
        }

        /*
            @brief The port actually bound, which is not always the one asked for.
            @details
            Requesting port 0 makes the OS choose; this is how a caller finds out
            what it got.  Zero before start() succeeds.
        */
        [[nodiscard]] auto port() const noexcept -> std::uint16_t
        {
            return this->bound_port_;
        }

    private:
        /* Only ever called with the accept thread joined or never started. */
        auto close_listener() noexcept -> void
        {
            chatwire::net::close_socket(this->listener_);
            this->listener_ = chatwire::net::invalid_socket;
        }

        auto cleanup_net() noexcept -> void
        {
            if (this->net_started_)
            {
                chatwire::net::cleanup();
                this->net_started_ = false;
            }
        }

        auto drop(const detail::client_ptr& c) noexcept -> void
        {
            if (!c) { return; }
            c->alive.store(false, std::memory_order_release);
            try
            {
                const std::lock_guard<std::mutex> guard{ this->clients_mutex_ };
                for (auto it{ this->clients_.begin() }; it != this->clients_.end(); ++it)
                {
                    if (*it == c) { this->clients_.erase(it); break; }
                }
            }
            catch (...)
            {
            }
        }

        /*
            @brief Accepts connections until stop() clears `running_`.
            @details
            Polled rather than blocked, with a timeout, so this thread owns its
            own exit: it never has to be woken by another thread closing the
            descriptor out from under it.  See stop() for why that mattered
            enough to restructure around.

            The interval is the worst-case delay added to shutdown, and shutdown
            is already waiting several ticks for hooks to drain, so a fifth of a
            second is invisible.  The cost of polling at that rate is one
            syscall every 200 ms on a thread that is otherwise asleep.
        */
        auto accept_loop() noexcept -> void
        {
            constexpr int poll_interval_ms{ 200 };

            while (this->running_.load(std::memory_order_acquire))
            {
                const int ready{ chatwire::net::wait_readable(this->listener_,
                                                              poll_interval_ms) };
                if (ready == 0) { continue; }               // nobody knocked
                if (ready < 0)
                {
                    if (chatwire::net::retryable(chatwire::net::last_error())) { continue; }
                    break;                                  // the listener is broken
                }

                const chatwire::net::socket_t sock{ ::accept(this->listener_, nullptr, nullptr) };
                if (sock == chatwire::net::invalid_socket) { continue; }

                // Between the poll and here, stop() may have run.  Refuse rather
                // than spawn a thread nothing is left to join us out of.
                if (!this->running_.load(std::memory_order_acquire))
                {
                    chatwire::net::close_socket(sock);
                    break;
                }

                detail::client_ptr c;
                try
                {
                    c = std::make_shared<detail::client>();
                }
                catch (...)
                {
                    chatwire::net::close_socket(sock);
                    continue;
                }
                c->sock = sock;
                // Assigned here, before the client is reachable by anything
                // else, and never reused: see client::id.
                c->id = this->next_id_.fetch_add(1, std::memory_order_relaxed);
                chatwire::net::prepare(sock);

                // Registered BEFORE the handshake, not after it.  stop() wakes
                // readers by walking this list, and a client that was still
                // reading its HTTP request would not have been on it -- so its
                // thread sat in recv() with nothing to wake it and the join
                // below never returned.  A game that will not close is a worse
                // bug than a connection counted a few milliseconds early.
                try
                {
                    const std::lock_guard<std::mutex> guard{ this->clients_mutex_ };
                    this->clients_.push_back(c);
                }
                catch (...) { continue; }         // ~client closes the socket

                try
                {
                    const std::lock_guard<std::mutex> guard{ this->threads_mutex_ };
                    this->reap_finished_threads();
                    this->client_threads_.emplace_back([this, c] { this->client_loop(c); });
                }
                catch (...)
                {
                    chatwire::log::warn("could not start a client thread; dropping connection");
                    this->drop(c);
                    continue;                     // ~client closes the socket
                }
            }
        }

        // Called under threads_mutex_.  Threads that finished are joined so the
        // vector does not grow without bound across a long session — the leak a
        // thread-per-client server acquires if nobody ever reaps.
        auto reap_finished_threads() noexcept -> void
        {
            if (this->client_threads_.size() < 32u) { return; }
            // Cheap approximation: if we have more thread objects than live
            // clients, some have exited and can be joined.
            std::size_t live{ 0 };
            try
            {
                const std::lock_guard<std::mutex> guard{ this->clients_mutex_ };
                live = this->clients_.size();
            }
            catch (...) { return; }

            if (this->client_threads_.size() > live * 2u + 8u)
            {
                for (auto& t : this->client_threads_)
                {
                    if (t.joinable()) { t.join(); }
                }
                this->client_threads_.clear();
            }
        }

        auto client_loop(const detail::client_ptr c) noexcept -> void
        {
            if (!c) { return; }

            if (!this->handshake(c))
            {
                this->drop(c);
                return;
            }

            // Already in `clients_`: accept_loop put it there so that stop()
            // could wake it out of the handshake.  Only the announcement waits
            // for the handshake to have succeeded.
            chatwire::log::info("client connected ({} total)", this->client_count());
            if (this->presence_) { this->presence_(true, c->id, this->client_count()); }

            std::vector<std::uint8_t> buffer;
            std::string               fragment;      // reassembled fragmented text
            std::array<char, 4096>    chunk{};

            while (this->running_.load(std::memory_order_acquire)
                   && c->alive.load(std::memory_order_acquire))
            {
                const std::ptrdiff_t n{ chatwire::net::recv_some(c->sock, chunk.data(),
                                                                 chunk.size()) };
                if (n == 0) { break; }               // peer closed, or stop() shut us down
                if (n < 0)
                {
                    if (chatwire::net::retryable(chatwire::net::last_error())) { continue; }
                    break;
                }

                try
                {
                    buffer.insert(buffer.end(), chunk.data(), chunk.data() + n);
                }
                catch (...)
                {
                    break;
                }

                // A peer that sends a huge partial frame must not grow this
                // buffer forever.  The frame decoder caps payloads at 16 MiB;
                // anything beyond that plus a header is malformed.
                if (buffer.size() > 17u * 1024u * 1024u) { break; }

                bool fatal{ false };
                for (;;)
                {
                    frame f{};
                    std::size_t consumed{ 0 };
                    const auto status{ decode(buffer, f, consumed) };
                    if (status == decode_status::incomplete) { break; }
                    if (status == decode_status::protocol_error) { fatal = true; break; }

                    buffer.erase(buffer.begin(),
                                 buffer.begin() + static_cast<std::ptrdiff_t>(consumed));

                    if (!this->dispatch(c, f, fragment)) { fatal = true; break; }
                }
                if (fatal) { break; }
            }

            this->drop(c);
            chatwire::log::info("client disconnected ({} remain)", this->client_count());
            if (this->presence_) { this->presence_(false, c->id, this->client_count()); }
        }

        /* @return false to close the connection. */
        auto dispatch(const detail::client_ptr& c, const frame& f, std::string& fragment) noexcept
            -> bool
        {
            switch (f.op)
            {
            case opcode::close:
                (void)c->send_frame(opcode::close, {});
                return false;

            case opcode::ping:
                return c->send_frame(opcode::pong, f.payload);

            case opcode::pong:
                return true;

            case opcode::continuation:
            case opcode::text:
            case opcode::binary:
            {
                try
                {
                    fragment += f.payload;
                }
                catch (...)
                {
                    return false;
                }
                if (!f.fin) { return true; }         // more to come

                std::string request;
                request.swap(fragment);

                if (!this->handler_) { return true; }
                std::string reply;
                try
                {
                    reply = this->handler_(c->id, request);
                }
                catch (...)
                {
                    // handler_ is noexcept, but a throwing allocation in the
                    // return value would still land here.
                    return true;
                }
                if (reply.empty()) { return true; }
                return c->send_frame(opcode::text, reply);
            }
            }
            return true;
        }

        auto handshake(const detail::client_ptr& c) noexcept -> bool
        {
            std::string request;
            std::array<char, 2048> chunk{};

            // Read until the header terminator.  Bounded: a peer that never
            // sends one must not be able to grow this.
            while (request.find("\r\n\r\n") == std::string::npos)
            {
                if (request.size() > 16u * 1024u) { return false; }
                if (!c->alive.load(std::memory_order_acquire)) { return false; }
                const std::ptrdiff_t n{ chatwire::net::recv_some(c->sock, chunk.data(),
                                                                 chunk.size()) };
                if (n == 0) { return false; }
                if (n < 0)
                {
                    if (chatwire::net::retryable(chatwire::net::last_error())) { continue; }
                    return false;
                }
                try { request.append(chunk.data(), static_cast<std::size_t>(n)); }
                catch (...) { return false; }
            }

            const auto key{ header_value(request, "sec-websocket-key") };
            if (key.empty())
            {
                static constexpr std::string_view bad{
                    "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n" };
                (void)chatwire::net::send_some(c->sock, bad.data(), bad.size());
                return false;
            }

            std::string reply;
            try
            {
                reply = "HTTP/1.1 101 Switching Protocols\r\n"
                        "Upgrade: websocket\r\n"
                        "Connection: Upgrade\r\n"
                        "Sec-WebSocket-Accept: " + accept_key(key) + "\r\n\r\n";
            }
            catch (...)
            {
                return false;
            }

            std::size_t sent{ 0 };
            while (sent < reply.size())
            {
                const std::ptrdiff_t n{ chatwire::net::send_some(
                    c->sock, reply.data() + sent, reply.size() - sent) };
                if (n <= 0)
                {
                    if (n < 0 && chatwire::net::retryable(chatwire::net::last_error()))
                    {
                        continue;
                    }
                    return false;
                }
                sent += static_cast<std::size_t>(n);
            }
            return true;
        }

        /* @brief Case-insensitive HTTP header lookup; HTTP header names are
           case-insensitive and browsers do not agree on casing. */
        [[nodiscard]] static auto header_value(const std::string_view request,
                                               const std::string_view lowercase_name)
            -> std::string
        {
            std::string lowered;
            try { lowered.reserve(request.size()); } catch (...) { return {}; }
            for (const char ch : request)
            {
                lowered += static_cast<char>((ch >= 'A' && ch <= 'Z') ? (ch - 'A' + 'a') : ch);
            }

            const std::size_t at{ lowered.find(std::string{ lowercase_name } + ":") };
            if (at == std::string::npos) { return {}; }

            std::size_t start{ at + lowercase_name.size() + 1u };
            while (start < request.size() && (request[start] == ' ' || request[start] == '\t'))
            {
                ++start;
            }
            const std::size_t end{ request.find("\r\n", start) };
            if (end == std::string::npos) { return {}; }
            return std::string{ request.substr(start, end - start) };
        }

        chatwire::net::socket_t         listener_{ chatwire::net::invalid_socket };
        /* Handed out by accept_loop().  Starts at 1 so that 0 is available as
           "no client", which is what an unrouted command carries. */
        std::atomic<std::uint64_t>      next_id_{ 1 };
        std::uint16_t                   bound_port_{ 0 };
        bool                            net_started_{ false };
        std::atomic<bool>               running_{ false };
        message_handler                 handler_{ nullptr };
        presence_handler                presence_{ nullptr };
        std::thread                     accept_thread_{};
        mutable std::mutex              clients_mutex_{};
        std::vector<detail::client_ptr> clients_{};
        std::mutex                      threads_mutex_{};
        std::vector<std::thread>        client_threads_{};
    };
}
