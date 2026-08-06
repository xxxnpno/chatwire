// dllmain — the Windows injection entry point.
//
// Everything both of them do once they are off the loader lock lives in
// src/entry.hpp, including the argument for why chatwire never unloads itself.
//
// ===========================================================================
// WHY DllMain DOES ALMOST NOTHING
// ===========================================================================
// DllMain runs under the loader lock.  Anything that waits, allocates through
// another DLL, starts a thread and joins it, or calls into the JVM can deadlock
// the whole process there — and chatwire's start-up does most of those.
//
// So DllMain does exactly two things: spawn threads, and return.  Every piece
// of real work happens on those threads, outside the lock.
//
// Standard headers first, then chatwire, then Windows: <windows.h> declares its
// world inside `extern "C"`, and a std declaration first seen from inside that
// block can pick up C language linkage.
#include "entry.hpp"

#include <windows.h>

namespace
{
    auto WINAPI startup_worker(LPVOID) noexcept -> DWORD
    {
        return chatwire::entry::start_now() ? 0u : 1u;
    }
}

/*
    @brief The name of the restart event for a given process.
    @details
    Per-process, in the Local namespace, so two games do not share one and no
    privilege is needed to open it.
*/
auto restart_event_name(const DWORD pid) -> std::string
{
    return "Local\\chatwire.restart." + std::to_string(pid);
}

namespace
{
    /*
        @brief Waits to be told to start chatwire again, forever.
        @details
        Detaching leaves the module MAPPED (see entry.hpp), so a second
        `chatwire-inject` cannot use LoadLibrary to wake us -- the loader sees the
        module already present and never runs DllMain again.

        The obvious fix, exporting a thread procedure for the injector to call
        with CreateRemoteThread, is a TRAP: the injector would have to compute the
        export's address, and the only cheap way is to read the RVA out of the DLL
        ON DISK.  After a rebuild that file is no longer the module that is
        mapped, so the computed address lands in the middle of unrelated code and
        takes the game with it.  It would work right up until the first rebuild.

        An event has no addresses in it.  The injector opens it by name and sets
        it; this thread, which is already inside the correct module, does the
        work.  Nothing crosses the process boundary except a signal.

        The thread is resident for the life of the process, which is only
        acceptable because chatwire no longer unloads -- a thread parked in a
        module that can be freed is the bug the module is kept mapped to avoid.
    */
    auto WINAPI supervisor_worker(LPVOID) noexcept -> DWORD
    {
        const std::string name{ restart_event_name(::GetCurrentProcessId()) };
        const HANDLE signal{ ::CreateEventA(nullptr, FALSE, FALSE, name.c_str()) };
        if (signal == nullptr) { return 1u; }

        for (;;)
        {
            if (::WaitForSingleObject(signal, INFINITE) != WAIT_OBJECT_0) { break; }
            if (chatwire::entry::g_started.load(std::memory_order_acquire)) { continue; }
            (void)startup_worker(nullptr);
        }
        ::CloseHandle(signal);
        return 0u;
    }
}

/*
    @brief Stops chatwire and releases everything it holds in the game.
    @details
    Exported so an injector, or any other tool, can shut chatwire down without
    going through the console.  Must NOT be called from DllMain — it starts a
    thread that joins other threads, and the loader lock makes that a deadlock.
*/
extern "C" __declspec(dllexport) auto chatwire_stop() -> void
{
    if (chatwire::entry::g_started.load(std::memory_order_acquire))
    {
        chatwire::entry::request_detach();
    }
}

/* @brief Whether chatwire is up.  For an injector to poll after injecting. */
extern "C" __declspec(dllexport) auto chatwire_is_running() -> int
{
    return chatwire::is_running() ? 1 : 0;
}

BOOL WINAPI DllMain(const HINSTANCE, const DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
    {
        // DisableThreadLibraryCalls USED TO BE HERE, and removing it is load
        // bearing.  It suppresses DLL_THREAD_DETACH, which is what this
        // toolchain's thread_local teardown rides on: MEASURED, chatwire.dll has
        // 31 __emutls symbols and no PE TLS directory, so GCC is using EMULATED
        // TLS and the destructors are driven by the DllMain notification rather
        // than by a TLS callback.
        //
        // vmhook releases a thread from the VM in exactly such a destructor.
        // Suppress the notification and every thread that ever called Java stays
        // registered as a JavaThread after its OS thread has died -- and the JVM
        // walks that list.  The symptom was a game that died a few seconds after
        // a detach that had reported success.
        //
        // The cost is a callback per thread, and Minecraft makes a lot of
        // threads.  It is a few instructions each; correctness is worth more.

        const HANDLE thread{ ::CreateThread(nullptr, 0, &startup_worker, nullptr, 0, nullptr) };
        if (thread != nullptr) { ::CloseHandle(thread); }

        // Listens for "start again" after a detach.  See supervisor_worker.
        const HANDLE supervisor{ ::CreateThread(nullptr, 0, &supervisor_worker,
                                                nullptr, 0, nullptr) };
        if (supervisor != nullptr) { ::CloseHandle(supervisor); }
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
