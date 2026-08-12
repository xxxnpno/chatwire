// chatwire.cpp — the bodies of start() / stop() and the wiring they use.
//
// Separate from the header because start() is large, runs exactly once, and has
// no business being emitted into every translation unit that calls it.  It is
// also the only place that pulls in the two heavyweight headers: sdk.hpp, which
// includes vmhook, and ws/server.hpp, which includes Winsock.  Everything else
// in chatwire sees neither.
//
// The startup ORDER is not arbitrary -- see the header of chatwire/chatwire.hpp.
//
// ws/server.hpp COMES FIRST, and has to.  It reaches winsock2.h, which refuses
// to be included after windows.h: it emits `#warning Please include winsock2.h
// before windows.h` and then defines a conflicting, older socket API.  sdk.hpp
// pulls in vmhook, which includes windows.h, so any order that puts sdk first
// loses the race -- and under -Werror that #warning is a build failure.
#include "chatwire/ws/server.hpp"

#include "chatwire/chatwire.hpp"

#include "chatwire/ansi.hpp"
#include "chatwire/console.hpp"
#include "chatwire/features/chat.hpp"
#include "chatwire/features/commands.hpp"
#include "chatwire/features/mapping.hpp"
#include "chatwire/features/system.hpp"
#include "chatwire/features/world.hpp"
#include "chatwire/sdk.hpp"

namespace chatwire::detail
{
    /*
        @brief The envelope every reply to a client comes in.
        @details
        `ok` is fixed by which struct this is, so neither is a field a caller
        fills in -- a reply that said `{"ok":false,...}` next to a result, or the
        reverse, was possible when the flag and the body were two arguments to
        one function.  See dispatch() below.
    */
    struct ok_reply
    {
        bool           ok{ true };
        json::verbatim result{};
    };
    struct error_reply
    {
        bool             ok{ false };
        std::string_view error{};
    };

    /*
        The server is a function-local static that is NEVER DESTROYED,
        deliberately.

        ~server joins threads, and static destruction of a DLL runs under the
        loader lock, where joining a thread is a guaranteed deadlock -- the game
        would hang on exit.  The hooks have the same shape of problem and vmhook
        owns that one: removing a detour touches a JVM that may already be
        tearing down, which is why nothing unhooks from a destructor and
        sdk::remove_hooks() calls vmhook::shutdown_hooks() explicitly.

        So it is leaked on purpose.  chatwire::stop() is the explicit,
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
        @brief The command sink: hands one command event to ONE client.
        @details
        The counterpart of broadcast_line, and deliberately not the same
        function.  A chat line is something that happened to everyone watching;
        a command belongs to the plugin that registered it, and delivering it
        anywhere else would let one connected tool act on another's commands.

        @return false when that client has gone, which the commands feature
                treats as "let the line through to the server" rather than as an
                error.
    */
    inline auto send_command_event(const std::uint64_t client,
                                   const std::string_view json_event) noexcept -> bool
    {
        return server_instance().send_to(client, json_event);
    }

    /*
        @brief Everything `system.stats` answers with, from every feature that
               counts something.
        @details
        Composed HERE rather than in `system`, so that neither counter-keeping
        feature has to include the other and `system` does not have to know how
        many there are.  Each feature returns its own counters as a struct, and
        json::object flattens the three into one object -- the features' fields
        laid out in this argument order.

        A fourth counter-keeping feature is one more argument on this line.  It
        used to be one more brace-less JSON fragment and one more `{}` in a
        format string that had to keep pace with it.
    */
    inline auto stats_json() -> std::string
    {
        return json::object(features::chat::stats(),
                            features::commands::stats(),
                            features::world::stats());
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
        server sees it.  Skipped entirely when the player is not in a world --
        there is no chat box on the title screen, and the message would go
        nowhere.

        Callable from whichever thread wanted to say something -- start-up says
        hello from the start-up thread, a connecting client's own socket thread
        announces it -- and SYNCHRONOUS either way: sdk::add_chat routes itself
        onto a thread allowed to touch Java and waits, so when this returns the
        message has either been shown or failed, and both are reported.  The
        version this replaces submitted to a queue and returned, which is why it
        could not tell anyone when nothing happened.

        Prefixed so it is obviously chatwire talking and not another player.
    */
    inline auto notify_in_game(const std::string_view text) noexcept -> void
    {
        try
        {
            // Both outcomes are REPORTED.  The earlier version discarded the
            // result, so an inject that landed on the title screen -- where
            // there is no chat box and no player to address -- looked exactly
            // like one that had worked, and the only symptom was a message that
            // never appeared.  A courtesy message failing is not worth a
            // warning, but it is worth a line.
            if (!sdk::in_world())
            {
                log::info("not in a world; skipped the in-game notice: {}", text);
                return;
            }
            // `{0}` four times: the section sign is ONE argument repeated, which
            // is what this line is actually saying and what four
            // std::string{...} temporaries hid.
            const std::string message{ std::format("{0}8[{0}bchatwire{0}8] {0}7{1}",
                                                   chatwire::ansi::section, text) };
            if (!sdk::add_chat(message))
            {
                log::warn("could not show the in-game notice: {}", text);
            }
        }
        catch (...) { }
    }

    /*
        @brief Announces a client connecting or disconnecting.
        @details
        Both in the console and in the player's own chat, because the person
        playing is the one who wants to know that something just attached to
        their game.

        Runs on that client's OWN socket thread, and now calls into Java from
        there.  The in-game half is skipped once chatwire is no longer running,
        which is not a detail: stop() joins these threads, so every one of them
        passes through here on the way out, and a shutdown that says goodbye
        once per connected client would enter the JVM as many times as there
        happen to be clients, while the hooks are coming down.  The console half
        still reports it -- that is chatwire talking to its operator, not to the
        game.
    */
    inline auto on_presence(const bool connected, const std::uint64_t client,
                            const std::size_t total) noexcept -> void
    {
        try
        {
            const bool live{ g_running.load(std::memory_order_acquire) };
            if (connected)
            {
                chatwire::console::event(std::format("client connected ({} total)", total));
                if (live) { notify_in_game(std::format("a client connected ({} total)", total)); }
            }
            else
            {
                // FIRST, and unconditionally: a plugin that has gone must stop
                // owning commands immediately, whether or not chatwire is still
                // running and before anything below can fail.  Leaving a name
                // claimed by a dead client means the player types `/ping`, the
                // line is swallowed, and nothing answers -- a game that has
                // quietly stopped working, with no error anywhere.
                features::commands::forget_client(client);

                chatwire::console::event(std::format("client disconnected ({} left)", total));
                if (live) { notify_in_game(std::format("a client disconnected ({} left)", total)); }

                // This is the last thing a client thread does before it exits,
                // and it may well have become a JavaThread on the way (any
                // command that reached the game attaches it).  Release it HERE
                // rather than trusting the thread_local teardown to fire: the
                // VM must not be left holding a JavaThread for an OS thread
                // that is about to disappear.
                sdk::detach_thread();
            }
        }
        catch (...) { }
    }

    /*
        @brief Routes one client message to the feature that owns it.
        @details
        `{"cmd":"net.minecraft.client.entity.EntityPlayerSP.sendChatMessage",
        "text":"hi"}` splits into the prefix
        `net.minecraft.client.entity.EntityPlayerSP` -- which the chat feature
        claims -- and the verb `sendChatMessage`.  Unknown prefixes and
        malformed messages get a shaped error rather than silence, because a
        client that gets nothing back cannot tell a typo from a hang.
    */
    inline auto dispatch(const std::uint64_t client, const std::string_view request) noexcept
        -> std::string
    {
        const auto reply{ [](const bool ok, const std::string& body) -> std::string
        {
            try
            {
                // json::verbatim, and this is the one place in chatwire that
                // uses it: `body` is a feature's finished reply and is JSON
                // already, so it goes in as it stands.  Written as a string it
                // would be quoted and escaped, and the client would get its
                // result as a lump of text to parse a second time.
                //
                // The two shapes are two structs rather than one with an unused
                // member, because that is what the protocol says: a reply
                // carries `result` OR `error`, never both and never an empty
                // one of the other.
                if (ok)
                {
                    return json::object(ok_reply{ .result = json::verbatim{ body } });
                }
                return json::object(error_reply{ .error = body });
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

            // The LAST dot, not the first.  A command is <prefix>.<verb>, and
            // the prefix is normally a fully-qualified Java class
            // ("net.minecraft.client.entity.EntityPlayerSP") -- which has dots
            // of its own.  Splitting at the first would make "net" the prefix
            // and the rest nonsense.  ("system" is the one bare prefix left,
            // and it works under the same rule.)
            const std::size_t dot{ cmd->rfind('.') };
            if (dot == std::string::npos || dot == 0u || dot + 1u >= cmd->size())
            {
                return reply(false, "'cmd' must look like <class>.<member>, e.g. "
                                    "net.minecraft.world.World.playerEntities");
            }

            const std::string feature_name{ cmd->substr(0, dot) };
            command parsed{};
            parsed.verb = cmd->substr(dot + 1u);
            parsed.body = request;
            // Carried through so a feature that registers something on the
            // caller's behalf knows whose behalf that is.  See command::client.
            parsed.client = client;

            feature* const target{ registry::find(feature_name) };
            if (!target)
            {
                return reply(false, std::format("no feature named '{}'", feature_name));
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
        @return false when no supported Minecraft was found, or the VM would not
                let this thread call Java — in which case nothing was left
                installed.
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
        //     rather than self-registering — see features/chat.hpp for why.
        registry::add(features::chat::instance());
        registry::add(features::commands::instance());
        registry::add(features::mapping::instance());
        registry::add(features::system::instance());
        registry::add(features::world::instance());

        // 3. Join the VM.  Everything past this point may call Java from this
        //    thread; nothing before it could.  This is where the pump used to
        //    be -- a detour on Minecraft.runTick, a queue, and a tick of
        //    latency, all of it in service of "a call must happen inside a
        //    hook".  It does not any more.
        //
        //    Failing here is fatal in the same way a missing Minecraft class
        //    is: chatwire could still observe chat, but nothing a client asked
        //    for would ever work, and a bridge that only listens is not the
        //    thing anyone injected.
        if (!sdk::attach_thread())
        {
            log::error("this thread could not join the VM; chatwire cannot call "
                       "into the game");
            return false;
        }

        // 3b. The safepoint gate and the caches every call depends on, resolved
        //     HERE, on a thread nothing is waiting for.  Doing either for the
        //     first time on the direct route would stop the game for the length
        //     of a class-graph walk.
        sdk::warm_up();

        if (!sdk::can_call_into_game())
        {
            // Not fatal -- observing chat still works, and that is most of what
            // chatwire does.  But every command will fail, so say so once, now.
            log::error("this JVM's collector relocates objects concurrently, so "
                       "vmhook will not enter Java on any thread; chat observation "
                       "works, commands will not");
        }

        // 3c. The command sink, BEFORE the features start.  Starting the
        //     commands feature installs its detour on sendChatMessage, and from
        //     that instant the game thread can be inside on_typed().  Nothing
        //     is registered yet, so it would find no owner and let the line
        //     through -- but installing the hook before its delivery route is a
        //     window that exists for no reason, and windows like that are how
        //     "it dropped exactly one message, once" bugs are made.
        features::commands::set_sink(&detail::send_command_event);
        // The world sink goes in here for the same reason and not one step
        // later: starting the world feature installs its detour on loadWorld,
        // and the game can be inside it before start_all() has returned.  A
        // sink installed afterwards would drop the world change that happened
        // while chatwire was coming up, which is precisely the one an injector
        // that ran on the title screen is waiting for.
        features::world::set_sink(&detail::broadcast_line);

        // 4. Features.  Installing a hook is metaspace work rather than a Java
        //    call, so it is safe on this thread and does not go through the
        //    pump -- and doing it here means a feature that fails is reported
        //    before the server opens rather than a tick later.
        {
            const std::size_t started{ registry::start_all() };
            log::info("{} feature(s) started", started);
        }

        // 5. The server, last, so an instant client finds a working API.
        features::chat::set_sink(&detail::broadcast_line);
        features::chat::set_console_sink(&detail::console_line);
        if (!detail::server_instance().start(bind_port, &detail::dispatch, &detail::on_presence))
        {
            log::error("websocket server failed to start; shutting down");
            features::commands::set_sink(nullptr);
            registry::stop_all();
            sdk::remove_hooks();
            // Same reason as the detach at the end of stop(): a failed start
            // ends in FreeLibraryAndExitThread on this very thread, and the VM
            // must not be left holding a JavaThread for it.
            sdk::detach_thread();
            return false;
        }

        detail::g_running.store(true, std::memory_order_release);

        features::system::set_status_port(detail::server_instance().port());
        features::system::set_can_call(sdk::can_call_into_game());
        features::system::set_client_counter(&chatwire::client_count);
        // `system.stats` answers with every counter-keeping feature's numbers,
        // joined here.  The host wires them together so that no feature has to
        // include another -- the numbers live where they are counted, and are
        // reported where the rest of chatwire's self-reporting is.
        features::system::set_stats_source(&detail::stats_json);

        chatwire::console::banner(chatwire::version, detail::server_instance().port(),
                                  mapping::mode_name(mode));
        // Just "attached".  The port belongs in the console banner and the log,
        // where someone is looking for it; in the chat box it is noise in front
        // of the one fact the player wants, which is that chatwire is in.
        detail::notify_in_game("attached");

        // The start-up thread returns to dllmain and exits straight after this,
        // and saying hello attached it.  Release it explicitly.
        sdk::detach_thread();
        return true;
    }

    /*
        @brief Takes chatwire down.  Safe to call more than once.
        @details
        Reverse of start(), and the order is what keeps unload from crashing:

          goodbye       — said first, while the hooks are still up
          server        — no new client work can arrive, and every client
                          thread is joined before anything is removed
          sink cleared  — the chat detour stops touching the server
          features      — whatever they installed comes down
          hooks         — last, so nothing is mid-detour when it goes
          this thread   — released from the VM, see below

        Every stage is bounded; nothing here can hang the game.  What made the
        old order delicate was the pump: teardown had to keep a queue open long
        enough for work already in it to run, and guess at a deadline for when
        the game would next tick.  Nothing here waits on the game any more --
        the calls happen on this thread, so they are finished when the line
        below them starts.
    */
    auto stop() noexcept -> void
    {
        if (!detail::g_running.exchange(false, std::memory_order_acq_rel)) { return; }

        log::info("chatwire stopping");

        // Said while the hooks are still up, and synchronously: when this
        // returns the player has been told.  The old version put the message in
        // a queue and then waited up to half a second hoping the game would
        // tick and drain it, which was the best it could do and still lost the
        // message on a paused or hitching client.
        /*
            THE GOODBYE RUNS ON A THREAD OF ITS OWN, AND THAT IS NOT DECORATION.

            Saying anything in-game means calling Java, and calling Java means
            becoming a JavaThread -- which registers a thread_local destructor
            (vmhook's exit_detacher) whose code lives in THIS DLL.  The thread
            running stop() is the detach worker, and it does not get to exit
            normally: it ends in FreeLibraryAndExitThread, unmapping the module
            that destructor lives in.  Letting it attach kills the game a moment
            after a detach that looked like it worked -- MEASURED, twice.

            A short-lived thread that is JOINED here has none of that problem: it
            attaches, speaks, and exits while the DLL is still loaded, so its
            thread_local teardown runs against mapped code and vmhook detaches it
            from the VM properly.  The unloading thread stays a plain native
            thread the JVM has never heard of, which is the only kind that is
            safe to unload from.
        */
        try
        {
            std::thread farewell{ []() noexcept
            {
                detail::notify_in_game("detached");
                // Explicit, for the same reason as the client threads.
                sdk::detach_thread();
            } };
            farewell.join();
        }
        catch (...) { }

        detail::server_instance().stop();
        features::chat::set_sink(nullptr);
        features::chat::set_console_sink(nullptr);
        features::world::set_sink(nullptr);
        // Cleared AFTER the server has gone, like the chat sink: while clients
        // still exist there is still somewhere for a command to be delivered,
        // and clearing this early would make the interceptor let a claimed
        // command through to the server instead.
        features::commands::set_sink(nullptr);
        features::system::set_client_counter(nullptr);
        features::system::set_stats_source(nullptr);

        registry::stop_all();

        // Hooks come down LAST: a thread still inside a detour must not have
        // that detour removed underneath it.
        sdk::remove_hooks();

        /*
            AND THEN WE WAIT, which is not optional and was learned the hard way:
            removing the hooks killed the game.

            The reasoning that omitted this was "by here the server has joined
            every client thread, so the only thread that could be inside a detour
            is this one".  That was true when the only hook was the chat
            observer, which fires when a message arrives.  It is false with the
            pump: its detour is on Minecraft.runTick, so THE GAME THREAD IS
            INSIDE OUR CODE TWENTY TIMES A SECOND, and nothing above joins the
            game thread -- it belongs to Minecraft and outlives us.

            remove_hooks() unpatches the entry point, so no thread ENTERS a
            trampoline afterwards.  It does not, and cannot, evict a thread that
            is in one already: vmhook keeps no in-flight count and offers no
            quiesce.  The caller of stop() then unloads the DLL immediately --
            FreeLibraryAndExitThread -- and any thread still executing a
            trampoline, a detour body or drain() is left running on unmapped
            pages.

            So: unpatch, then let everyone leave.  A detour body is microseconds
            and a tick is 50 ms, so this is many times longer than it needs to be
            on purpose -- the cost is a pause nobody sees during a teardown that
            was already asynchronous, and the alternative is the game dying a
            moment after a detach that looked like it worked.  Waiting is the
            only tool available; the honest fix is an in-flight count in vmhook,
            and this is what stands in for it until there is one.
        */
        std::this_thread::sleep_for(std::chrono::milliseconds{ 500 });

        // Let go of the VM explicitly.  An attached thread is normally detached
        // by vmhook when it exits -- but THIS thread does not get to exit
        // normally: it is the detach worker, and it ends in
        // FreeLibraryAndExitThread, which unmaps the module the thread-exit
        // handler lives in.  Detaching here means the VM is not left holding a
        // JavaThread for a thread that is about to vanish, whose stack a
        // safepoint would later try to walk.
        sdk::detach_thread();

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
