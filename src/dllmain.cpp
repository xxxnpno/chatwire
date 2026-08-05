// dllmain — the injection entry point.
//
// ===========================================================================
// WHY DllMain DOES ALMOST NOTHING
// ===========================================================================
// DllMain runs under the loader lock.  Anything that waits, allocates through
// another DLL, starts a thread and joins it, or calls into the JVM can deadlock
// the whole process there — and chatwire's start-up does most of those.
//
// So DllMain does exactly two things: spawn a thread, and return.  Every piece
// of real work happens on that thread, outside the lock.
//
// Unloading is driven from the other direction.  DLL_PROCESS_DETACH also holds
// the loader lock, so it cannot join anything either; instead the detach path
// stops chatwire on a thread of its own and finishes with
// FreeLibraryAndExitThread, which is the one supported way for a DLL to unload
// itself — it drops the reference and exits the thread atomically, so the
// thread can never still be executing code in a module that has been freed.
//
// Standard headers first, then chatwire, then Windows: <windows.h> declares its
// world inside `extern "C"`, and a std declaration first seen from inside that
// block can pick up C language linkage.
#include "chatwire/chatwire.hpp"
#include "chatwire/config.hpp"
#include "chatwire/console.hpp"

#include <windows.h>

namespace
{
    /* Our own module handle, captured in DllMain: needed to find the config file
       beside the DLL, and to unload ourselves. */
    HMODULE g_module{ nullptr };

    std::atomic<bool> g_started{ false };

    /*
        @brief Where chatwire's settings come from, in order of precedence.
        @details
        1. `chatwire.cfg` beside the DLL, written by the injector immediately
           before injecting and CONSUMED here.  This is how --port and
           --background reach a game the injector did not start: a process's
           environment is fixed at creation, so a flag cannot be delivered as an
           environment variable to something already running.
        2. CHATWIRE_PORT / CHATWIRE_BACKGROUND / CHATWIRE_VERBOSE in the game's
           own environment, for anyone launching the game themselves.
        3. The defaults.
    */
    auto load_settings() noexcept -> chatwire::config::settings
    {
        auto s{ chatwire::config::consume(chatwire::config::path_for(g_module)) };

        const auto env_flag{ [](const char* const name, bool& out) noexcept
        {
            char        buffer[16]{};
            const DWORD n{ ::GetEnvironmentVariableA(name, buffer, sizeof(buffer)) };
            if (n == 0u || n >= sizeof(buffer)) { return; }
            out = (buffer[0] != '0');
        } };

        if (s.port == 0u)
        {
            char        buffer[16]{};
            const DWORD n{ ::GetEnvironmentVariableA("CHATWIRE_PORT", buffer, sizeof(buffer)) };
            if (n > 0u && n < sizeof(buffer))
            {
                std::uint32_t parsed{ 0 };
                bool          digits{ true };
                for (DWORD i{ 0 }; i < n; ++i)
                {
                    if (buffer[i] < '0' || buffer[i] > '9') { digits = false; break; }
                    parsed = parsed * 10u + static_cast<std::uint32_t>(buffer[i] - '0');
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

        bool background{ !s.console };
        env_flag("CHATWIRE_BACKGROUND", background);
        s.console = !background;
        env_flag("CHATWIRE_VERBOSE", s.verbose);

        if (s.port == 0u) { s.port = chatwire::default_port; }
        return s;
    }

    /*
        @brief Stops chatwire, releases the console, and unloads this DLL.
        @details
        On its own thread, NOT on the loader lock, because chatwire::stop()
        joins threads.  See the file header for why the exit is
        FreeLibraryAndExitThread rather than a plain return.
    */
    auto WINAPI detach_worker(LPVOID) noexcept -> DWORD
    {
        chatwire::stop();
        chatwire::console::release();
        g_started.store(false, std::memory_order_release);

        if (g_module != nullptr) { ::FreeLibraryAndExitThread(g_module, 0); }
        return 0u;
    }

    /* @brief What the console's `detach` command calls.  Must not block it. */
    auto request_detach() noexcept -> void
    {
        const HANDLE thread{ ::CreateThread(nullptr, 0, &detach_worker, nullptr, 0, nullptr) };
        if (thread != nullptr) { ::CloseHandle(thread); }
    }

    auto WINAPI startup_worker(LPVOID) noexcept -> DWORD
    {
        const auto settings{ load_settings() };

        chatwire::log::set_level(settings.verbose ? chatwire::log::level::info
                                                  : chatwire::log::level::warning);

        // The console has to exist BEFORE start(), so the banner and any warning
        // raised during start-up have somewhere to go.
        if (settings.console && !chatwire::console::attach(&request_detach))
        {
            chatwire::log::warn("could not attach a console; running without one");
        }

        if (!chatwire::start(settings.port))
        {
            chatwire::log::error("chatwire could not start; unloading");
            chatwire::console::release();
            if (g_module != nullptr)
            {
                // Unload ourselves, so a failed injection leaves nothing behind
                // and the user can simply try again.
                ::FreeLibraryAndExitThread(g_module, 1);
            }
            return 1u;
        }

        g_started.store(true, std::memory_order_release);
        return 0u;
    }
}

/*
    @brief Stops chatwire and unloads the DLL.
    @details
    Exported so an injector, or any other tool, can shut chatwire down without
    going through the console.  Must NOT be called from DllMain — it starts a
    thread that joins other threads, and the loader lock makes that a deadlock.
*/
extern "C" __declspec(dllexport) auto chatwire_stop() -> void
{
    if (g_started.load(std::memory_order_acquire)) { request_detach(); }
}

/* @brief Whether chatwire is up.  For an injector to poll after injecting. */
extern "C" __declspec(dllexport) auto chatwire_is_running() -> int
{
    return chatwire::is_running() ? 1 : 0;
}

BOOL WINAPI DllMain(const HINSTANCE module_handle, const DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
    {
        g_module = static_cast<HMODULE>(module_handle);

        // Thread attach/detach notifications are useless to us and cost a
        // callback on every JVM thread — and Minecraft makes a lot of threads.
        ::DisableThreadLibraryCalls(module_handle);

        const HANDLE thread{ ::CreateThread(nullptr, 0, &startup_worker, nullptr, 0, nullptr) };
        if (thread != nullptr) { ::CloseHandle(thread); }
        break;
    }

    case DLL_PROCESS_DETACH:
        // The loader lock is held, so nothing that joins may run here.  The
        // supported ways out are the console's `detach` command and the exported
        // chatwire_stop(), both of which unload from a thread of their own.
        break;

    default:
        break;
    }
    return TRUE;
}
