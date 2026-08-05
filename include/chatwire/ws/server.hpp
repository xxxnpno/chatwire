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
//   * stop() closes the listener FIRST, which wakes the accept thread out of
//     accept() with an error rather than leaving it blocked forever;
//   * it then shuts down every client socket, waking their readers the same way;
//   * it JOINS every thread before returning, so no thread can still be inside
//     this module when the DLL unloads;
//   * client threads never touch the client list except through the mutex, and
//     they remove themselves before exiting;
//   * a client that stops reading cannot stall the game: writes are best-effort
//     and a failed write closes that client rather than blocking the broadcaster.
#include "chatwire/common.hpp"

#include "chatwire/log.hpp"
#include "chatwire/ws/websocket.hpp"

// Platform headers strictly after the standard ones.
#include <winsock2.h>
#include <ws2tcpip.h>
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
        SOCKET     sock{ INVALID_SOCKET };
        std::mutex write_mutex{};              // serialises frames on this socket
        std::atomic<bool> alive{ true };

        ~client()
        {
            if (sock != INVALID_SOCKET) { ::closesocket(sock); }
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
                    const int n{ ::send(sock,
                                        reinterpret_cast<const char*>(frame.data() + sent),
                                        static_cast<int>(frame.size() - sent), 0) };
                    if (n <= 0) { alive.store(false, std::memory_order_release); return false; }
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
    */
    using message_handler = std::string (*)(std::string_view request) noexcept;

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
        [[nodiscard]] auto start(const std::uint16_t port, const message_handler handler) noexcept
            -> bool
        {
            if (this->running_.load(std::memory_order_acquire)) { return true; }
            this->handler_ = handler;

            WSADATA wsa{};
            if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
            {
                chatwire::log::error("WSAStartup failed");
                return false;
            }
            this->wsa_started_ = true;

            this->listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (this->listener_ == INVALID_SOCKET)
            {
                chatwire::log::error("socket() failed: {}", ::WSAGetLastError());
                this->cleanup_winsock();
                return false;
            }

            // Without this, a restart inside the same game session hits
            // TIME_WAIT and fails to bind for ~2 minutes.
            BOOL reuse{ TRUE };
            (void)::setsockopt(this->listener_, SOL_SOCKET, SO_REUSEADDR,
                               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port   = ::htons(port);
            // Loopback ONLY.  See the file header.
            addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);

            if (::bind(this->listener_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR)
            {
                chatwire::log::error("bind(127.0.0.1:{}) failed: {}", port, ::WSAGetLastError());
                this->close_listener();
                this->cleanup_winsock();
                return false;
            }
            if (::listen(this->listener_, SOMAXCONN) == SOCKET_ERROR)
            {
                chatwire::log::error("listen() failed: {}", ::WSAGetLastError());
                this->close_listener();
                this->cleanup_winsock();
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
                this->cleanup_winsock();
                return false;
            }

            chatwire::log::info("websocket server listening on ws://127.0.0.1:{}", port);
            return true;
        }

        /*
            @brief Stops accepting, drops every client, joins every thread.
            @details
            Idempotent, and safe to call from the destructor.  The ORDER is the
            point: closing the listener and shutting down client sockets is what
            wakes threads blocked in accept()/recv(); without that, join() would
            hang forever and the game would freeze on unload.
        */
        auto stop() noexcept -> void
        {
            if (!this->running_.exchange(false, std::memory_order_acq_rel))
            {
                // Never started, or already stopped.  Still join in case start()
                // failed halfway.
                if (this->accept_thread_.joinable()) { this->accept_thread_.join(); }
                return;
            }

            this->close_listener();

            // Wake every client reader.  shutdown() rather than closesocket():
            // the reader still owns the handle and will close it on the way out,
            // and closing it here would risk the handle being reused underneath.
            {
                const std::lock_guard<std::mutex> guard{ this->clients_mutex_ };
                for (auto& c : this->clients_)
                {
                    if (c)
                    {
                        c->alive.store(false, std::memory_order_release);
                        (void)::shutdown(c->sock, SD_BOTH);
                    }
                }
            }

            if (this->accept_thread_.joinable()) { this->accept_thread_.join(); }

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

            this->cleanup_winsock();
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

    private:
        auto close_listener() noexcept -> void
        {
            if (this->listener_ != INVALID_SOCKET)
            {
                ::closesocket(this->listener_);
                this->listener_ = INVALID_SOCKET;
            }
        }

        auto cleanup_winsock() noexcept -> void
        {
            if (this->wsa_started_)
            {
                ::WSACleanup();
                this->wsa_started_ = false;
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

        auto accept_loop() noexcept -> void
        {
            while (this->running_.load(std::memory_order_acquire))
            {
                const SOCKET sock{ ::accept(this->listener_, nullptr, nullptr) };
                if (sock == INVALID_SOCKET)
                {
                    // stop() closed the listener; that is the normal exit.
                    if (!this->running_.load(std::memory_order_acquire)) { break; }
                    continue;
                }

                detail::client_ptr c;
                try
                {
                    c = std::make_shared<detail::client>();
                }
                catch (...)
                {
                    ::closesocket(sock);
                    continue;
                }
                c->sock = sock;

                try
                {
                    const std::lock_guard<std::mutex> guard{ this->threads_mutex_ };
                    this->reap_finished_threads();
                    this->client_threads_.emplace_back([this, c] { this->client_loop(c); });
                }
                catch (...)
                {
                    chatwire::log::warn("could not start a client thread; dropping connection");
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

            try
            {
                const std::lock_guard<std::mutex> guard{ this->clients_mutex_ };
                this->clients_.push_back(c);
            }
            catch (...)
            {
                this->drop(c);
                return;
            }
            chatwire::log::info("client connected ({} total)", this->client_count());

            std::vector<std::uint8_t> buffer;
            std::string               fragment;      // reassembled fragmented text
            std::array<char, 4096>    chunk{};

            while (this->running_.load(std::memory_order_acquire)
                   && c->alive.load(std::memory_order_acquire))
            {
                const int n{ ::recv(c->sock, chunk.data(), static_cast<int>(chunk.size()), 0) };
                if (n <= 0) { break; }               // peer closed, or stop() shut us down

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
                    reply = this->handler_(request);
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
                const int n{ ::recv(c->sock, chunk.data(), static_cast<int>(chunk.size()), 0) };
                if (n <= 0) { return false; }
                try { request.append(chunk.data(), static_cast<std::size_t>(n)); }
                catch (...) { return false; }
            }

            const auto key{ header_value(request, "sec-websocket-key") };
            if (key.empty())
            {
                static constexpr std::string_view bad{
                    "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n" };
                (void)::send(c->sock, bad.data(), static_cast<int>(bad.size()), 0);
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
                const int n{ ::send(c->sock, reply.data() + sent,
                                    static_cast<int>(reply.size() - sent), 0) };
                if (n <= 0) { return false; }
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

        SOCKET                          listener_{ INVALID_SOCKET };
        bool                            wsa_started_{ false };
        std::atomic<bool>               running_{ false };
        message_handler                 handler_{ nullptr };
        std::thread                     accept_thread_{};
        mutable std::mutex              clients_mutex_{};
        std::vector<detail::client_ptr> clients_{};
        std::mutex                      threads_mutex_{};
        std::vector<std::thread>        client_threads_{};
    };
}
