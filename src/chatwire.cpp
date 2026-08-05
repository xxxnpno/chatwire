// chatwire.cpp — the bodies of start() / stop() and the wiring they use.
//
// Separate from the header because start() is large, runs exactly once, and has
// no business being emitted into every translation unit that calls it.  It is
// also the only place that pulls in the two heavyweight headers: sdk.hpp, which
// includes vmhook, and ws/server.hpp, which includes Winsock.  Everything else
// in chatwire sees neither.
//
// The startup ORDER is not arbitrary -- see the header of chatwire/chatwire.hpp.
#include "chatwire/chatwire.hpp"

#include "chatwire/ansi.hpp"
#include "chatwire/console.hpp"
#include "chatwire/features/chat.hpp"
#include "chatwire/features/system.hpp"
#include "chatwire/sdk.hpp"
#include "chatwire/ws/server.hpp"

namespace chatwire::detail
{
    /*
        The server and the pump hook are function-local statics that are NEVER
        DESTROYED, deliberately.

        Both destructors do things that must not happen during static
        destruction or DLL unload: ~server joins threads, and ~hook_handle
        removes a detour from a JVM that may already be tearing down.  Static
        destruction of a DLL runs under the loader lock, where joining a thread
        is a guaranteed deadlock -- the game would hang on exit.

        So they are leaked on purpose.  chatwire::stop() is the explicit,
        correctly-ordered teardown, and the process reclaims everything anyway.
        This also removes the atexit registration that a namespace-scope object
        with a destructor would need, which GCC 15's module machinery does not
        currently get right.
    */
    inline auto server_instance() noexcept -> ws::server&
    {
        static auto* const s{ new ws::server{} };
        return *s;
    }

    inline std::atomic<bool> g_running{ false };

    /* The chat sink: hands every observed line to every connected client. */
    inline auto broadcast_line(const std::string_view json_line) noexcept -> void
    {
        server_instance().broadcast(json_line);
    }

    /*
        @brief Shows one chat line in chatwire's own console.
        @details
        Separate from the broadcast so the console keeps working when nothing is
        connected -- watching chat scroll past is a use on its own, not just a
        debugging aid for the socket.
    */
    inline auto console_line(const std::string_view formatted) noexcept -> void
    {
        chatwire::console::chat_line(formatted);
    }

    /*
        @brief Says something in the player's own chat box.
        @details
        Client-side only (addChatMessage), so nothing is transmitted and no
        server sees it.  Queued onto the pump because it has to run on the game
        thread, and skipped entirely when the player is not in a world -- there
        is no chat box on the title screen, and the message would be lost rather
        than delayed.

        Prefixed so it is obviously chatwire talking and not another player.
    */
    inline auto notify_in_game(std::string text) noexcept -> void
    {
        (void)pump::submit([message = std::move(text)]() noexcept
        {
            if (!sdk::in_world()) { return; }
            (void)sdk::add_chat("Â§""8[Â§""bchatwireÂ§""8] Â§""7" + message);
        });
    }

    /*
        @brief Announces a client connecting or disconnecting.
        @details
        Both in the console and in the player's own chat, because the person
        playing is the one who wants to know that something just attached to
        their game.
    */
    inline auto on_presence(const bool connected, const std::size_t total) noexcept -> void
    {
        try
        {
            if (connected)
            {
                chatwire::console::event(std::format("client connected ({} total)", total));
                notify_in_game(std::format("a client connected ({} total)", total));
            }
            else
            {
                chatwire::console::event(std::format("client disconnected ({} left)", total));
                notify_in_game(std::format("a client disconnected ({} left)", total));
            }
        }
        catch (...) { }
    }

    /*
        @brief Routes one client message to the feature that owns it.
        @details
        `{"cmd":"chat.send","text":"hi"}` splits into feature "chat", verb
        "send".  Unknown features and malformed messages get a shaped error
        rather than silence, because a client that gets nothing back cannot tell
        a typo from a hang.
    */
    inline auto dispatch(const std::string_view request) noexcept -> std::string
    {
        const auto reply{ [](const bool ok, const std::string& body) -> std::string
        {
            try
            {
                return json::object(json::field("ok", ok) + ","
                                    + (ok ? "\"result\":" + body
                                          : json::field("error", body)));
            }
            catch (...)
            {
                return R"({"ok":false,"error":"internal error"})";
            }
        } };

        try
        {
            const auto cmd{ json::get_string(request, "cmd") };
            if (!cmd) { return reply(false, "missing or non-string 'cmd'"); }

            const std::size_t dot{ cmd->find('.') };
            if (dot == std::string::npos || dot == 0u || dot + 1u >= cmd->size())
            {
                return reply(false, "'cmd' must look like feature.verb");
            }

            const std::string feature_name{ cmd->substr(0, dot) };
            command parsed{};
            parsed.verb = cmd->substr(dot + 1u);
            parsed.body = request;

            feature* const target{ registry::find(feature_name) };
            if (!target)
            {
                return reply(false, "no feature named '" + feature_name + "'");
            }

            const response result{ target->handle(parsed) };
            return reply(result.ok, result.json_body);
        }
        catch (...)
        {
            return reply(false, "internal error");
        }
    }
}

namespace chatwire
{
    /*
        @brief Brings chatwire up.  Call from an injected thread, not DllMain.
        @details
        Blocks until the JVM is ready (bounded by `timeout`), then installs
        everything.  Safe to call once; a second call while running is a no-op.

        @param port     TCP port on 127.0.0.1.
        @param timeout  How long to wait for Minecraft's classes to appear.
                        A client that is still on the launcher screen has a JVM
                        but no Minecraft class yet.
        @return false when no supported Minecraft was found, or the pump could
                not be installed — in which case nothing was left installed.
    */
    auto start(const std::uint16_t port, const std::chrono::seconds timeout) noexcept
        -> bool
    {
        if (detail::g_running.load(std::memory_order_acquire)) { return true; }

        // Port 0 asks the OS for ANY free port, which is a legitimate thing to
        // want but never what a caller who simply did not set one means.  A
        // caller that genuinely wants an ephemeral port can read the bound one
        // back from the log.
        const std::uint16_t bind_port{ port == 0u ? default_port : port };

        log::info("chatwire {} starting (vmhook {}.{}.{})", chatwire::version,
                  VMHOOK_VERSION_MAJOR, VMHOOK_VERSION_MINOR, VMHOOK_VERSION_PATCH);

        // 1. Wait for Minecraft, and work out which mapping this build uses.
        const auto deadline{ std::chrono::steady_clock::now() + timeout };
        mapping::mode mode{ mapping::mode::unknown };
        while (std::chrono::steady_clock::now() < deadline)
        {
            mode = sdk::detect_mapping();
            if (mode != mapping::mode::unknown) { break; }
            std::this_thread::sleep_for(std::chrono::milliseconds{ 500 });
        }
        if (mode == mapping::mode::unknown)
        {
            log::error("no supported Minecraft 1.8.9 found; chatwire is not starting");
            return false;
        }
        log::info("mapping detected: {}", mapping::mode_name(mode));

        // 2. Register the wrappers under this mapping's class names.
        if (!sdk::register_all()) { return false; }

        // 2b. Register the features.  ADDING A FEATURE IS TWO LINES: the import
        //     at the top of this file, and one registry::add here.  Explicit
        //     rather than self-registering — see features/chat.ixx for why.
        registry::add(features::chat::instance());
        registry::add(features::system::instance());

        // 3. Deoptimise BOTH hook targets in one class-graph walk, before either
        //    hook is installed.  Doing it per-hook meant walking every loaded
        //    class twice, which on a modded client is seconds each time.
        sdk::deoptimize_hook_targets();

        // 4. The pump.  Everything after this needs a way onto the game thread.
        pump::reopen();
        if (!sdk::install_pump(&pump::drain))
        {
            log::error("could not hook Minecraft.runTick; chatwire cannot reach "
                       "the game thread");
            return false;
        }

        // 5. Features, on the game thread.  Hooking resolves klasses, which has
        //    the same thread requirements as any other JVM work.
        {
            std::atomic<bool> done{ false };
            (void)pump::submit([&done]() noexcept
            {
                const std::size_t started{ registry::start_all() };
                log::info("{} feature(s) started", started);
                done.store(true, std::memory_order_release);
            });

            // Bounded wait.  If the pump never fires (game not ticking yet) we
            // carry on: the features will start on the first tick that happens,
            // and the server can already answer `stats`.
            const auto feature_deadline{ std::chrono::steady_clock::now()
                                         + std::chrono::seconds{ 30 } };
            while (!done.load(std::memory_order_acquire)
                   && std::chrono::steady_clock::now() < feature_deadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds{ 50 });
            }
            if (!done.load(std::memory_order_acquire))
            {
                log::warn("features have not started yet (is the game ticking?); "
                          "they will start on the first tick");
            }
        }

        // 6. The server, last, so an instant client finds a working API.
        features::chat::set_sink(&detail::broadcast_line);
        features::chat::set_console_sink(&detail::console_line);
        if (!detail::server_instance().start(bind_port, &detail::dispatch, &detail::on_presence))
        {
            log::error("websocket server failed to start; shutting down");
            (void)pump::submit([]() noexcept { registry::stop_all(); });
            pump::shutdown();
            sdk::remove_hooks();
            return false;
        }

        detail::g_running.store(true, std::memory_order_release);

        features::system::set_status_port(detail::server_instance().port());

        chatwire::console::banner(chatwire::version, detail::server_instance().port(),
                                  mapping::mode_name(mode));
        detail::notify_in_game("connected - listening on 127.0.0.1:"
                               + std::to_string(detail::server_instance().port()));
        return true;
    }

    /*
        @brief Takes chatwire down.  Safe to call more than once.
        @details
        Reverse of start(), and the order is what keeps unload from crashing:

          server first  — no new client work can arrive
          sink cleared  — the chat detour stops touching the server
          features      — hooks come down, on the game thread
          pump          — closed only after the tasks that needed it have run

        Every stage is bounded; nothing here can hang the game.
    */
    auto stop() noexcept -> void
    {
        if (!detail::g_running.exchange(false, std::memory_order_acq_rel)) { return; }

        log::info("chatwire stopping");

        // Say goodbye while the hooks are still up -- afterwards there is no way
        // onto the game thread to say anything at all.  Bounded wait: if the
        // game is not ticking, shutdown must not hang on a courtesy message.
        detail::notify_in_game("detaching - goodbye");
        {
            const auto deadline{ std::chrono::steady_clock::now()
                                 + std::chrono::milliseconds{ 500 } };
            while (pump::snapshot().pending > 0u
                   && std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds{ 25 });
            }
        }

        detail::server_instance().stop();
        features::chat::set_sink(nullptr);
        features::chat::set_console_sink(nullptr);

        {
            std::atomic<bool> done{ false };
            (void)pump::submit([&done]() noexcept
            {
                registry::stop_all();
                done.store(true, std::memory_order_release);
            });
            const auto deadline{ std::chrono::steady_clock::now() + std::chrono::seconds{ 5 } };
            while (!done.load(std::memory_order_acquire)
                   && std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds{ 25 });
            }
            if (!done.load(std::memory_order_acquire))
            {
                log::warn("features did not stop in time; unhooking anyway");
            }
        }

        pump::shutdown();
        // Hooks come down LAST, in one pass: a task still running in a detour
        // must not have its detour removed underneath it.
        sdk::remove_hooks();
        log::info("chatwire stopped");
    }

    /* @brief How many WebSocket clients are connected. */
    auto client_count() noexcept -> std::size_t
    {
        return detail::server_instance().client_count();
    }

    auto is_running() noexcept -> bool
    {
        return detail::g_running.load(std::memory_order_acquire);
    }
}
