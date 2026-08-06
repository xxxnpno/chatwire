#pragma once

// chatwire.features.commands — commands added to Minecraft at RUNTIME.
//
// ===========================================================================
// WHAT IT DOES
// ===========================================================================
// A connected client says "from now on, /ping is mine":
//
//     {"cmd":"commands.register","name":"ping"}
//
// From that moment, typing `/ping` in the game's chat box does NOT go to the
// server.  chatwire swallows the line and pushes an event to the client that
// registered it:
//
//     {"type":"net.minecraft.client.entity.EntityPlayerSP.sendChatMessage",
//      "command":"ping","args":["a","b"],"raw":"/ping a b"}
//
// What happens next is entirely the client's business, and it is an ordinary
// chatwire command like any other:
//
//     {"cmd":"net.minecraft.client.entity.EntityPlayerSP.addChatMessage",
//      "text":"pong"}
//
// That round trip IS the plugin system.  There is no plugin format, no manifest
// and nothing to compile against: a plugin is a program that holds a socket
// open, and it is added and removed while the game runs.
//
// ===========================================================================
// WHY THIS SHAPE AND NOT A SIMPLER ONE
// ===========================================================================
// The obvious cheaper design is a canned reply -- register `/ping` WITH the
// text "pong" and let chatwire answer by itself, no round trip and no socket
// write on the game thread.  It was rejected because it is a language: the
// moment somebody wants an argument in the reply, or a lookup, or a condition,
// the canned string grows placeholders, then conditionals, and chatwire ends up
// owning a small interpreter nobody asked for.  Handing the event to a real
// program written in a real language costs one round trip on loopback and can
// never need a feature.
//
// The second thing this deliberately is NOT: a broadcast.  A command belongs to
// the client that registered it, and its events go there alone.  Broadcasting
// would be fewer lines and wrong twice -- every other connected tool would see
// commands it did not register, and two plugins claiming `/ping` would both
// act on it.  Ownership is also what makes withdrawal automatic: when a client
// disconnects, its commands go with it (see forget_client), so a plugin that
// crashes cannot leave `/ping` swallowed forever with nobody left to answer.
//
// ===========================================================================
// WHY IT KEEPS A SHORT PREFIX
// ===========================================================================
// Every command that touches the game spells out the Java member it reaches,
// so a reader can check it against Minecraft's source.  `commands.register`
// reaches nothing in the game: it writes a name into a table chatwire owns.
// There is no member to name it after and nothing for a reader to check, which
// is the same reason `system` keeps its short prefix -- and the only reason
// either of them is allowed to.
//
// The EVENT is named the other way round, after the method it comes out of,
// exactly like the chat event is named after printChatMessage.  It shares that
// name with a command a client can send, and that is not a collision: `type` is
// what happened in the game, `cmd` is what you are asking for, and they are
// different keys.
//
// ===========================================================================
// THREADING
// ===========================================================================
// on_typed() runs INSIDE the sendChatMessage detour, on the game thread.
// handle() runs on a client's socket thread.  Both touch the registration
// table, so it is behind a mutex -- and the rule that keeps that mutex from
// being a way to freeze Minecraft is:
//
//     NEVER hold the lock across a socket write or a call into Java.
//
// Everything below copies what it needs out from under the lock and acts after
// releasing it.  A socket thread that entered Java while holding this lock
// could be stopped at a safepoint the game thread is waiting to reach, with the
// game thread blocked on the lock behind it -- which is a deadlock made of two
// things that are each individually correct.
#include "chatwire/common.hpp"

#include "chatwire/command_line.hpp"
#include "chatwire/feature.hpp"
#include "chatwire/json.hpp"
#include "chatwire/log.hpp"
#include "chatwire/sdk.hpp"

namespace chatwire::features
{
    /*
        @brief Where a command event goes: to ONE client, by id.
        @details
        A plain function pointer installed by the host, the same arrangement the
        chat feature uses for its sink -- this feature must not know that a
        client is a socket, or it could not be reasoned about without one.

        @return false when that client is no longer connected.
    */
    using command_sink = bool (*)(std::uint64_t client, std::string_view json) noexcept;

    inline std::atomic<command_sink> g_command_sink{ nullptr };
    inline std::atomic<std::uint64_t> g_commands_run{ 0 };
    inline std::atomic<std::uint64_t> g_commands_dropped{ 0 };

    namespace commands_detail
    {
        /* One claimed name, and who claimed it. */
        struct registration
        {
            std::string   name{};
            std::uint64_t client{ 0 };
        };

        /*
            @brief The claimed names.  NEVER DESTROYED, on purpose.
            @details
            Leaked exactly like the feature registry and the hook list, and for
            the same reason: destruction would run during static teardown or DLL
            unload, and the game thread can be inside on_typed() reading this at
            any moment up until the hooks come down.

            A vector rather than a map: the scan happens once per line the
            PLAYER types, there will be a handful of entries, and preserving
            registration order makes `commands.list` read the way a plugin
            author registered them.
        */
        [[nodiscard]] inline auto table() noexcept -> std::vector<registration>&
        {
            static auto* const t{ new std::vector<registration>{} };
            return *t;
        }

        [[nodiscard]] inline auto table_mutex() noexcept -> std::mutex&
        {
            static auto* const m{ new std::mutex{} };
            return *m;
        }

        /*
            A ceiling, so one confused client cannot make every line the player
            types walk a table of ten thousand names on the game thread.  It is
            not a security boundary -- the socket is loopback-only and anything
            that can reach it can send chat as the player anyway -- it is a
            bound on the work done in a detour.
        */
        inline constexpr std::size_t max_registrations{ 256 };

        // The three text decisions -- what a typed line invokes, what its
        // arguments are, and what a registerable name looks like -- live in
        // chatwire/command_line.hpp, spelled out in full at every use below.
        // They are the only part of this feature that means anything without a
        // JVM, which is why they are somewhere a test can reach them, and where
        // chatwire-mock takes them from rather than keeping a second copy of
        // the same rules to drift out of step.
    }

    class commands_feature final : public chatwire::feature
    {
    public:
        [[nodiscard]] auto name() const noexcept -> std::string_view override
        {
            return "commands";
        }

        // claims() is left at its default -- "my name and nothing else" -- which
        // is the right answer for a feature that reaches nothing in Minecraft.
        // See the note at the top of this file.

        [[nodiscard]] auto start() noexcept -> bool override
        {
            if (!chatwire::sdk::install_command_interceptor(&on_typed))
            {
                chatwire::log::warn("commands: could not intercept sent chat; "
                                    "runtime commands are unavailable");
                this->intercepting_.store(false, std::memory_order_release);
                return false;
            }
            this->intercepting_.store(true, std::memory_order_release);
            return true;
        }

        auto stop() noexcept -> void override
        {
            // The hook itself comes down centrally, in one pass, after every
            // thread that could be inside it has been joined -- see
            // chatwire::stop().  What this has to do is stop CLAIMING names, so
            // that a line typed between here and the unhook goes to the server
            // the way it would if chatwire had never been injected.
            this->intercepting_.store(false, std::memory_order_release);
            try
            {
                const std::lock_guard<std::mutex> guard{ commands_detail::table_mutex() };
                commands_detail::table().clear();
            }
            catch (...) { }
        }

        [[nodiscard]] auto handle(const chatwire::command& cmd) noexcept
            -> chatwire::response override
        {
            try
            {
                if (cmd.verb == "register")   { return this->do_register(cmd); }
                if (cmd.verb == "unregister") { return this->do_unregister(cmd); }
                if (cmd.verb == "list")       { return do_list(); }

                return chatwire::response::failure(
                    "unknown verb; try register, unregister or list");
            }
            catch (...)
            {
                return chatwire::response::failure("internal error");
            }
        }

    private:
        [[nodiscard]] auto do_register(const chatwire::command& cmd) noexcept
            -> chatwire::response
        {
            if (!this->intercepting_.load(std::memory_order_acquire))
            {
                return chatwire::response::failure(
                    "the command interceptor is not installed; nothing typed in "
                    "the game would ever reach this command");
            }
            // Refused rather than registered in nobody's name: an owner is what
            // makes the event deliverable and the withdrawal automatic.
            if (cmd.client == 0u)
            {
                return chatwire::response::failure(
                    "no client to own this command");
            }

            const auto asked{ chatwire::json::get_string(cmd.body, "name") };
            if (!asked) { return chatwire::response::failure("missing or non-string 'name'"); }

            const std::string_view clean{ chatwire::command_line::normalise(*asked) };
            if (clean.empty())
            {
                return chatwire::response::failure(
                    "'name' must be 1-64 characters with no spaces; a single "
                    "leading '/' is allowed and ignored");
            }
            const std::string name{ clean };

            {
                const std::lock_guard<std::mutex> guard{ commands_detail::table_mutex() };
                auto& table{ commands_detail::table() };

                const auto existing{ std::find_if(table.begin(), table.end(),
                    [&](const auto& e) noexcept { return e.name == name; }) };

                if (existing != table.end())
                {
                    // Re-registering something you already own is a no-op rather
                    // than an error: a plugin reconnecting and replaying its
                    // whole list should not have to care which half survived.
                    if (existing->client != cmd.client)
                    {
                        return chatwire::response::failure(
                            "'" + name + "' is already registered by another client");
                    }
                }
                else
                {
                    if (table.size() >= commands_detail::max_registrations)
                    {
                        return chatwire::response::failure("too many registered commands");
                    }
                    table.push_back(commands_detail::registration{
                        .name = name, .client = cmd.client });
                }
            }

            chatwire::log::info("commands: client {} registered /{}", cmd.client, name);
            return chatwire::response::success(chatwire::json::object(
                chatwire::json::field("registered", name)));
        }

        [[nodiscard]] auto do_unregister(const chatwire::command& cmd) noexcept
            -> chatwire::response
        {
            const auto asked{ chatwire::json::get_string(cmd.body, "name") };
            if (!asked) { return chatwire::response::failure("missing or non-string 'name'"); }

            const std::string_view clean{ chatwire::command_line::normalise(*asked) };
            if (clean.empty()) { return chatwire::response::failure("'name' is not a command name"); }
            const std::string name{ clean };

            bool removed{ false };
            {
                const std::lock_guard<std::mutex> guard{ commands_detail::table_mutex() };
                auto& table{ commands_detail::table() };
                for (auto it{ table.begin() }; it != table.end(); ++it)
                {
                    if (it->name != name) { continue; }
                    // Only the owner may withdraw it.  Anything else is one
                    // connected tool being able to silently break another.
                    if (it->client != cmd.client)
                    {
                        return chatwire::response::failure(
                            "'" + name + "' belongs to another client");
                    }
                    table.erase(it);
                    removed = true;
                    break;
                }
            }

            if (!removed)
            {
                return chatwire::response::failure("'" + name + "' is not registered");
            }
            return chatwire::response::success(chatwire::json::object(
                chatwire::json::field("unregistered", name)));
        }

        [[nodiscard]] static auto do_list() -> chatwire::response
        {
            std::vector<commands_detail::registration> snapshot;
            {
                const std::lock_guard<std::mutex> guard{ commands_detail::table_mutex() };
                snapshot = commands_detail::table();
            }

            std::string entries;
            for (const auto& entry : snapshot)
            {
                if (!entries.empty()) { entries += ","; }
                entries += chatwire::json::object(
                    chatwire::json::field("name", entry.name) + ","
                    + chatwire::json::field("client",
                        static_cast<std::int64_t>(entry.client)));
            }

            return chatwire::response::success(chatwire::json::object(
                chatwire::json::field("count", static_cast<std::int64_t>(snapshot.size()))
                + ",\"commands\":[" + entries + "]"));
        }

        /*
            @brief One line the player sent.  INSIDE THE DETOUR, on the game
                   thread.
            @details
            Returns true to swallow it.  Everything here is bounded and none of
            it enters Java: a table scan under a mutex, some string work, and one
            socket write.  The write is the only part that can take real time,
            and it is the same exposure the chat observer already has when it
            broadcasts from printChatMessage -- a peer that has stopped reading
            can stall whoever is writing to it, and here that is Minecraft's own
            thread.  Loopback with a handful of local tools is what makes that
            acceptable; it is not a property that would survive this socket being
            reachable from anywhere else, which is one more reason it is not.

            The lock is released BEFORE the write.  See the threading note at the
            top of this file for what holding it across one would risk.
        */
        [[nodiscard]] static auto on_typed(const char* const raw) noexcept -> bool
        {
            try
            {
                if (!raw) { return false; }
                const std::string_view line{ raw };

                const std::string_view invoked{ chatwire::command_line::invoked_name(line) };
                if (invoked.empty()) { return false; }      // not a slash command

                std::uint64_t owner{ 0 };
                {
                    const std::lock_guard<std::mutex> guard{ commands_detail::table_mutex() };
                    for (const auto& entry : commands_detail::table())
                    {
                        if (entry.name == invoked) { owner = entry.client; break; }
                    }
                }
                if (owner == 0u) { return false; }          // nobody claimed it

                const command_sink sink{ g_command_sink.load(std::memory_order_acquire) };
                if (!sink)
                {
                    // Registered but undeliverable.  Let the line through rather
                    // than eating it: a command that vanishes silently is
                    // indistinguishable from a broken game.
                    g_commands_dropped.fetch_add(1, std::memory_order_relaxed);
                    return false;
                }

                std::string args;
                for (const auto& argument : chatwire::command_line::arguments(line))
                {
                    if (!args.empty()) { args += ","; }
                    args += "\"" + chatwire::json::escape(argument) + "\"";
                }

                // Named after the method this comes OUT of, exactly as the chat
                // event is named after printChatMessage.
                const std::string payload{ chatwire::json::object(
                    chatwire::json::field(
                        "type", "net.minecraft.client.entity.EntityPlayerSP.sendChatMessage")
                    + "," + chatwire::json::field("command", invoked)
                    + ",\"args\":[" + args + "]"
                    + "," + chatwire::json::field("raw", line)) };

                if (!sink(owner, payload))
                {
                    // The owner went away between the lookup and the write.  Its
                    // registrations are about to be dropped by forget_client, so
                    // this line is the last one that could be eaten -- let it go
                    // to the server instead.
                    g_commands_dropped.fetch_add(1, std::memory_order_relaxed);
                    return false;
                }

                g_commands_run.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
            catch (...)
            {
                // Whatever went wrong, the safe direction is to let the player's
                // message reach the server.
                return false;
            }
        }

        std::atomic<bool> intercepting_{ false };
    };
}

namespace chatwire::features::commands
{
    /* @brief Installs where a command event is delivered.  See command_sink. */
    inline auto set_sink(const chatwire::features::command_sink sink) noexcept -> void
    {
        chatwire::features::g_command_sink.store(sink, std::memory_order_release);
    }

    /*
        @brief Drops everything registered by `client`.
        @details
        Called when that client disconnects, and it is the reason a registration
        has an owner at all.  Without it a plugin that crashed would leave its
        commands claimed forever: the player would type `/ping`, chatwire would
        swallow the line, and nothing would answer -- a game that has quietly
        stopped working with no error anywhere.

        Safe to call for a client that registered nothing, which is the usual
        case.
    */
    inline auto forget_client(const std::uint64_t client) noexcept -> void
    {
        if (client == 0u) { return; }
        try
        {
            std::size_t dropped{ 0 };
            {
                const std::lock_guard<std::mutex> guard{
                    chatwire::features::commands_detail::table_mutex() };
                auto& table{ chatwire::features::commands_detail::table() };
                const auto first_gone{ std::remove_if(table.begin(), table.end(),
                    [client](const auto& entry) noexcept { return entry.client == client; }) };
                // Subtraction rather than std::distance: these are vector
                // iterators, so it is the same answer without <iterator>, which
                // chatwire/common.hpp does not include.
                dropped = static_cast<std::size_t>(table.end() - first_gone);
                table.erase(first_gone, table.end());
            }
            if (dropped != 0u)
            {
                chatwire::log::info("commands: client {} left; {} command(s) withdrawn",
                                    client, dropped);
            }
        }
        catch (...) { }
    }

    /*
        @brief The counters, as a JSON body fragment for `system.stats`.
        @details
        Lives here because the numbers do.  `run` is commands delivered to a
        plugin; `dropped` is commands that matched but could not be delivered
        and were therefore let through to the server -- a number worth having,
        because from inside the game that outcome looks exactly like a command
        that was never registered.
    */
    [[nodiscard]] inline auto stats_json() -> std::string
    {
        return chatwire::json::field("commands_run",
                   static_cast<std::int64_t>(
                       chatwire::features::g_commands_run.load(std::memory_order_relaxed)))
            + "," + chatwire::json::field("commands_dropped",
                   static_cast<std::int64_t>(
                       chatwire::features::g_commands_dropped.load(std::memory_order_relaxed)));
    }

    /* @brief This feature's singleton, for the root module to register. */
    [[nodiscard]] inline auto instance() noexcept -> chatwire::feature*
    {
        static chatwire::features::commands_feature feature{};
        return &feature;
    }
}
