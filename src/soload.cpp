// soload — the Linux and macOS entry point.
//
// Compiled off Windows only; src/dllmain.cpp is the Windows counterpart.
// Everything both of them do once they are off the loader lock lives in
// src/entry.hpp, including the argument for why chatwire never unloads itself.
//
// ===========================================================================
// HOW CHATWIRE GETS IN HERE
// ===========================================================================
// On Windows chatwire is injected into a game that is ALREADY RUNNING, with
// CreateRemoteThread and LoadLibrary.  Neither has a POSIX equivalent that
// works the same way for the same effort -- see tools/injector.cpp and the
// README -- so the supported route here is the loader's own:
//
//     LD_PRELOAD=/path/to/chatwire.so        java -jar ...     (Linux)
//     DYLD_INSERT_LIBRARIES=/path/chatwire.dylib  java -jar ... (macOS)
//
// which means chatwire is present from the process's first instruction, long
// before there is a JVM, never mind a Minecraft.  That is not a problem to work
// around: chatwire::start() already polls for Minecraft's classes with a
// timeout, because on Windows it had to cope with being injected into a game
// still sitting on its launcher screen.  The same loop covers "the JVM has not
// been created yet".  What it does need is a longer default patience, which is
// what CHATWIRE_TIMEOUT is for.
//
// ===========================================================================
// AN INITIALISER IS A LOADER CALLBACK
// ===========================================================================
// This function runs while the dynamic loader holds dl_load_lock, which is the
// same hazard as DllMain's loader lock under a different name: anything that
// waits, joins, or loads another library from here can deadlock the process.
// So it spawns a thread and returns, exactly as DllMain does.
//
// One consequence worth stating, because it is the reason there is no
// "supervisor" thread here to match the Windows one: after `system.detach`,
// chatwire stays mapped but stopped, and a second LD_PRELOAD is not a thing
// that can happen to a running process.  dlopen()ing this library again would
// only bump a reference count -- initialisers do not re-run -- so on POSIX
// "start it again" means restarting the game.  The Windows build has a named
// event for this precisely because it HAS an injector to set it.
#include "entry.hpp"

#include <pthread.h>

namespace
{
    /*
        @brief The start-up thread.  Everything real happens here.
        @details
        A raw pthread rather than std::thread, and it is not styling: a
        std::thread object created in an initialiser has to be stored somewhere
        and then detached, which means a namespace-scope object with a
        destructor running at exit, in a library that is deliberately still
        mapped.  A detached pthread has no such object.  Its trampoline is code
        in this module, which is only safe because chatwire never unloads -- the
        same reason the Windows side is allowed to leave a supervisor parked
        here forever.
    */
    extern "C" auto startup_thread(void*) -> void*
    {
        (void)chatwire::entry::start_now();
        return nullptr;
    }

    /*
        @brief Runs when the loader maps this library.  UNDER dl_load_lock.
        @details
        pthread_create is one of the few substantial things that is safe from an
        initialiser: it does not load a library and does not wait on the loader.
        Starting the thread is all that happens here.

        Failure is silent by design.  There is nothing to report it to -- the
        game's stderr belongs to the game, and a preload that could not start a
        thread has bigger problems than a missing chat bridge -- and an
        initialiser that abort()s takes the game down with it.
    */
    __attribute__((constructor)) auto chatwire_on_load() -> void
    {
        ::pthread_attr_t attributes{};
        if (::pthread_attr_init(&attributes) != 0) { return; }
        // Detached at creation: nothing here will ever join it, and a joinable
        // thread nobody joins is a leak of its stack for the life of the game.
        (void)::pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);

        ::pthread_t thread{};
        (void)::pthread_create(&thread, &attributes, &startup_thread, nullptr);
        (void)::pthread_attr_destroy(&attributes);
    }
}

/*
    @brief Stops chatwire and releases everything it holds in the game.
    @details
    Exported so a tool that has dlopen()ed this library, or a debugger, can shut
    chatwire down without going through the websocket.  Returns immediately; the
    shutdown happens on a thread of its own.
*/
extern "C" __attribute__((visibility("default"))) auto chatwire_stop() -> void
{
    if (chatwire::entry::g_started.load(std::memory_order_acquire))
    {
        chatwire::entry::request_detach();
    }
}

/* @brief Whether chatwire is up.  Mirrors the Windows export of the same name. */
extern "C" __attribute__((visibility("default"))) auto chatwire_is_running() -> int
{
    return chatwire::is_running() ? 1 : 0;
}
