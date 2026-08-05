#pragma once

// entry — everything the two loader entry points have in common.
//
// chatwire is woken by its platform's loader: DllMain on Windows, an ELF/Mach-O
// initialiser on Linux and macOS.  Those two are genuinely different and get a
// file each (src/dllmain.cpp, src/soload.cpp).  Everything they then DO is the
// same, and lives here rather than being written twice and drifting.
//
// The rule both entry points obey is also the same, and it is the only thing
// about loader callbacks worth memorising: THEY RUN UNDER THE LOADER LOCK.
// Anything that waits, starts a thread and joins it, allocates through another
// module, or calls into the JVM can deadlock the whole process there -- and
// chatwire's start-up does most of those.  So each entry point spawns a thread
// and returns, and every piece of real work happens on that thread, outside the
// lock.  Windows calls it the loader lock and POSIX calls it dl_load_lock; the
// hazard is identical.
#include "chatwire/chatwire.hpp"
#include "chatwire/config.hpp"
#include "chatwire/console.hpp"
#include "chatwire/features/system.hpp"
#include "chatwire/log.hpp"

#include <cstdlib>

#if defined(_WIN32)
    #include <windows.h>
#endif

namespace chatwire::entry
{
    inline std::atomic<bool> g_started{ false };

    namespace detail
    {
        /* @brief An environment flag: absent leaves `out` alone, "0" is false. */
        inline auto env_flag(const char* const name, bool& out) noexcept -> void
        {
            if (const char* const value{ std::getenv(name) }; value != nullptr
                && value[0] != '\0')
            {
                out = (value[0] != '0');
            }
        }
    }

    /*
        @brief Where chatwire's settings come from, in order of precedence.
        @details
        1. `chatwire.cfg` beside the library, written by the injector immediately
           before injecting and CONSUMED here.  This is how --port and
           --background reach a game the injector did not start: a process's
           environment is fixed at creation, so a flag cannot be delivered as an
           environment variable to something already running.
        2. CHATWIRE_PORT / CHATWIRE_CONSOLE / CHATWIRE_BACKGROUND /
           CHATWIRE_VERBOSE in the game's own environment, for anyone launching
           the game themselves -- which on Linux and macOS is the ordinary way
           in, since chatwire arrives there through LD_PRELOAD or
           DYLD_INSERT_LIBRARIES on a command line the user wrote.
        3. The defaults.
    */
    [[nodiscard]] inline auto load_settings() noexcept -> chatwire::config::settings
    {
        auto s{ chatwire::config::consume(chatwire::config::path_for_self()) };

        if (s.port == 0u)
        {
            if (const char* const text{ std::getenv("CHATWIRE_PORT") }; text != nullptr)
            {
                std::uint32_t parsed{ 0 };
                bool          digits{ text[0] != '\0' };
                for (const char* c{ text }; *c != '\0'; ++c)
                {
                    if (*c < '0' || *c > '9') { digits = false; break; }
                    parsed = parsed * 10u + static_cast<std::uint32_t>(*c - '0');
                    if (parsed > 65535u) { digits = false; break; }
                }
                // 0 is a legal port to BIND -- it means "any free one" -- which
                // makes it a terrible sentinel for "unset".  Treated as unset,
                // because that is what someone writing 0 into an env var means.
                if (digits && parsed != 0u) { s.port = static_cast<std::uint16_t>(parsed); }
                else if (!digits)
                {
                    chatwire::log::warn("CHATWIRE_PORT is not a valid port; using {}",
                                        chatwire::default_port);
                }
            }
        }

        // The console is OPT-IN, matching the injector's default.  Both spellings
        // are honoured because both read naturally depending on which way round
        // you are thinking about it.
        detail::env_flag("CHATWIRE_CONSOLE", s.console);
        bool background{ !s.console };
        detail::env_flag("CHATWIRE_BACKGROUND", background);
        s.console = !background;

        detail::env_flag("CHATWIRE_VERBOSE", s.verbose);

        if (const char* const text{ std::getenv("CHATWIRE_TIMEOUT") }; text != nullptr)
        {
            std::uint32_t parsed{ 0 };
            bool          digits{ text[0] != '\0' };
            for (const char* c{ text }; *c != '\0'; ++c)
            {
                if (*c < '0' || *c > '9') { digits = false; break; }
                parsed = parsed * 10u + static_cast<std::uint32_t>(*c - '0');
                if (parsed > 86400u) { digits = false; break; }
            }
            if (digits && parsed != 0u) { s.timeout_seconds = parsed; }
        }

        if (s.port == 0u) { s.port = chatwire::default_port; }
        return s;
    }

    /*
        @brief Stops chatwire and releases the console.  On its own thread.
        @details
        Never on the caller's: chatwire::stop() joins every client thread, and
        the websocket thread asking to detach is one of them -- it would join
        itself and deadlock.

        THE MODULE IS DELIBERATELY NOT UNLOADED.

        Detaching used to end in FreeLibraryAndExitThread, and that killed the
        game: MEASURED as EXCEPTION_ACCESS_VIOLATION with "data execution
        prevention violation", on Minecraft's own Server thread, in
        `_thread_in_Java`, jumping to an address that was no longer executable.
        That is a thread which was inside a detour trampoline when the pages
        under it went away.

        Removing a hook stops new threads ENTERING it.  It cannot evict a thread
        that is already inside, and vmhook keeps no in-flight count to wait on,
        so there is no instant at which unloading is provably safe while the game
        is running.  Waiting first only shrinks the window.

        The argument is not Windows-specific and neither is the conclusion:
        dlclose() on Linux and macOS unmaps exactly the same way, and a thread
        inside a trampoline dies exactly the same way.  So chatwire stops but
        stays mapped on all three.  What that costs is about a megabyte of
        address space until the game exits.  What it buys is that `system.detach`
        cannot kill the process it is detaching from -- and everything observable
        is gone either way: the socket is closed, the hooks are out, and start()
        can be called again.
    */
    inline auto detach_worker() noexcept -> void
    {
        // A moment first, so a `system.detach` reply still has a live server to
        // travel over.  The wait lives HERE rather than on the websocket thread
        // that asked for it, because this thread is allowed to be slow and that
        // one is holding up a client's request.
        std::this_thread::sleep_for(std::chrono::milliseconds{ 300 });

        chatwire::stop();
        chatwire::console::release();
        g_started.store(false, std::memory_order_release);
    }

    /*
        @brief What `system.detach` and the console's `detach` both call.
        @details
        Returns immediately; the work happens on a thread of its own.  Must not
        block, because one of its callers is a websocket thread mid-request.
    */
    inline auto request_detach() noexcept -> void
    {
#if defined(_WIN32)
        // A raw CreateThread rather than a detached std::thread, and that is a
        // deliberate holdover: std::thread's trampoline is code in THIS module,
        // so a detached one is a thread executing here after the object that
        // spawned it is gone.  chatwire no longer unloads, which makes it safe
        // either way -- but this path is the one that used to crash games, and
        // it is not the place to spend a "should be fine".
        const ::HANDLE thread{ ::CreateThread(
            nullptr, 0,
            [](::LPVOID) noexcept -> ::DWORD { detach_worker(); return 0u; },
            nullptr, 0, nullptr) };
        if (thread != nullptr) { (void)::CloseHandle(thread); }
#else
        try
        {
            std::thread{ &detach_worker }.detach();
        }
        catch (...)
        {
            chatwire::log::error("could not start the detach thread; chatwire is "
                                 "still running");
        }
#endif
    }

    /*
        @brief Brings chatwire up.  Runs on a thread, never on the loader lock.
        @return false when chatwire could not start; the module stays loaded
                either way, so a retry is possible without re-injecting.
    */
    inline auto start_now() noexcept -> bool
    {
        const auto settings{ load_settings() };

        chatwire::log::set_level(settings.verbose ? chatwire::log::level::info
                                                  : chatwire::log::level::warning);

        // The same detach path serves the console's `detach` command and the
        // websocket's `system.detach`.  Both need a thread of their own -- see
        // request_detach -- and neither may run on the caller's.
        chatwire::features::system::set_detach_handler(&request_detach);

        // The console has to exist BEFORE start(), so the banner and any warning
        // raised during start-up have somewhere to go.
        if (settings.console && !chatwire::console::attach(&request_detach))
        {
            chatwire::log::warn("could not attach a console; running without one");
        }

        // 0 means the caller said nothing, so chatwire::start's own default
        // applies.  Repeating that number here would be a second place to keep
        // it in step with the first.
        const bool ok{ settings.timeout_seconds == 0u
                           ? chatwire::start(settings.port)
                           : chatwire::start(settings.port,
                                             std::chrono::seconds{ settings.timeout_seconds }) };
        if (!ok)
        {
            chatwire::log::error("chatwire could not start");
            chatwire::console::release();
            return false;
        }

        g_started.store(true, std::memory_order_release);
        return true;
    }
}
