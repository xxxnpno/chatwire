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
// Unload is the mirror image.  DLL_PROCESS_DETACH also holds the loader lock,
// and joining a thread from there is a guaranteed deadlock, so detach only
// returns.  The DLL is expected to stay loaded for the life of the game, which
// is the normal case for an injected library; a caller wanting a clean shutdown
// calls the exported chatwire_stop() first.
//
// ===========================================================================
// WHY THERE IS NO `import` HERE
// ===========================================================================
// GCC 15 cannot compile a TU that both imports a module and includes
// <windows.h>.  This file needs <windows.h>; everything else needs the module.
// So the two never meet: chatwire.entry defines a small C ABI, and this file
// declares it by hand below.  See src/entry.ixx.
#include <cstdio>

#include <windows.h>

// ---------------------------------------------------------------------------
// The chatwire C ABI, declared by hand.  Defined in src/entry.ixx.  Keep these
// four signatures in lockstep with that file — they are matched by the linker,
// not by the compiler, so a mismatch is a runtime crash rather than a build
// error.  There are only four, and they are not expected to grow.
// ---------------------------------------------------------------------------
extern "C" int  chatwire_bootstrap(unsigned short port);
extern "C" void chatwire_shutdown(void);
extern "C" int  chatwire_running(void);
extern "C" void chatwire_log(const char* message);

namespace
{
    /*
        @brief Reads the port from CHATWIRE_PORT, else 0 for "use the default".
        @details
        An environment variable rather than a config file: an injector already
        controls the environment of the process it starts, and a file would be
        another thing to find, parse and get wrong at start-up.

        Parsed strictly.  A malformed value falls back rather than being
        half-read: `strtol` alone would turn "80abc" into 80, and binding a port
        the user did not ask for is worse than ignoring their typo.
    */
    auto configured_port() noexcept -> unsigned short
    {
        char        buffer[16]{};
        const DWORD n{ ::GetEnvironmentVariableA("CHATWIRE_PORT", buffer, sizeof(buffer)) };
        if (n == 0u || n >= sizeof(buffer)) { return 0u; }

        unsigned long value{ 0u };
        for (DWORD i{ 0 }; i < n; ++i)
        {
            const char c{ buffer[i] };
            if (c < '0' || c > '9')
            {
                chatwire_log("CHATWIRE_PORT is not a number; using the default port");
                return 0u;
            }
            value = value * 10u + static_cast<unsigned long>(c - '0');
            if (value > 65535u)
            {
                chatwire_log("CHATWIRE_PORT is out of range; using the default port");
                return 0u;
            }
        }
        return static_cast<unsigned short>(value);
    }

    auto WINAPI worker(const LPVOID module_handle) noexcept -> DWORD
    {
        // A console makes the log visible when injected into a javaw.exe that
        // has none.  Failure is fine — the game just runs without visible logs.
        if (::GetConsoleWindow() == nullptr && ::AllocConsole())
        {
            FILE* stream{ nullptr };
            (void)::freopen_s(&stream, "CONOUT$", "w", stdout);
            (void)::freopen_s(&stream, "CONOUT$", "w", stderr);
        }

        if (chatwire_bootstrap(configured_port()) == 0)
        {
            chatwire_log("chatwire could not start; unloading");
            if (module_handle != nullptr)
            {
                // Unload ourselves so a failed injection leaves nothing behind
                // and the user can simply try again.
                ::FreeLibraryAndExitThread(static_cast<HMODULE>(module_handle), 1);
            }
            return 1u;
        }
        return 0u;
    }
}

/*
    @brief Stops chatwire and lets the DLL be unloaded.
    @details
    Exported so an injector can shut chatwire down cleanly rather than relying on
    process exit.  Must NOT be called from DllMain.
*/
extern "C" __declspec(dllexport) auto chatwire_stop() -> void
{
    chatwire_shutdown();
}

/* @brief Whether chatwire is up.  For an injector to poll after injecting. */
extern "C" __declspec(dllexport) auto chatwire_is_running() -> int
{
    return chatwire_running();
}

BOOL WINAPI DllMain(const HINSTANCE module_handle, const DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
    {
        // Thread attach/detach notifications are useless to us and cost a
        // callback on every JVM thread — and Minecraft makes a lot of threads.
        ::DisableThreadLibraryCalls(module_handle);

        const HANDLE thread{ ::CreateThread(nullptr, 0, &worker, module_handle, 0, nullptr) };
        if (thread != nullptr) { ::CloseHandle(thread); }
        break;
    }

    case DLL_PROCESS_DETACH:
        // The loader lock is held.  chatwire_shutdown() joins threads, which
        // would deadlock here, so it is deliberately not called.  A caller
        // wanting a clean shutdown uses the exported chatwire_stop() first.
        break;

    default:
        break;
    }
    return TRUE;
}
