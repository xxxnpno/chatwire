// chatwire.entry — the C ABI the injection entry point calls through.
//
// ===========================================================================
// WHY THIS LAYER EXISTS
// ===========================================================================
// GCC 15 cannot currently compile a translation unit that both imports a module
// and includes <windows.h>: it either rejects the std declarations that reach it
// through the import ("conflicting language linkage", because <windows.h>
// declares its world inside extern "C") or, with the ordering fixed, segfaults
// outright.
//
// dllmain.cpp needs <windows.h>.  Everything else needs the module.  So they are
// separated, and this file is the seam:
//
//     dllmain.cpp        entry.ixx              the rest of chatwire
//     -----------        ---------              --------------------
//     <windows.h>        import chatwire;       modules all the way down
//     hand-written  ---> extern "C" defs   ---> chatwire::start / stop
//     declarations
//
// dllmain declares these three by hand — three lines, no import, no module
// machinery — and the linker matches them because they have C language linkage.
//
// The seam is worth keeping even once the compiler bug is gone: the entry point
// having no C++ dependency on the library is what lets an injector load the DLL
// and call it without agreeing on anything but the ABI.
module;

// The shared preamble, FIRST and identical in every module.  See the header
// for why GCC 15 requires that of a modular build.
#include "core/prelude.hpp"

export module chatwire.entry;

import chatwire;

/*
    @brief Starts chatwire on `port`.
    @details
    Blocks until the JVM is ready or the attempt times out, so the caller must
    run it on its own thread — never on the loader lock.
    @return 1 on success, 0 on failure.
*/
extern "C" auto chatwire_bootstrap(const unsigned short port) -> int
{
    return chatwire::start(port == 0u ? chatwire::default_port : port) ? 1 : 0;
}

/*
    @brief Stops chatwire: server down, hooks removed, threads joined.
    @details
    Must NOT be called from DllMain — it joins threads, and the loader lock makes
    that a deadlock.
*/
extern "C" auto chatwire_shutdown() -> void
{
    chatwire::stop();
}

/* @brief 1 while chatwire is up. */
extern "C" auto chatwire_running() -> int
{
    return chatwire::is_running() ? 1 : 0;
}

/* @brief Connected WebSocket clients.  For an injector to report status. */
extern "C" auto chatwire_client_count() -> int
{
    return static_cast<int>(chatwire::client_count());
}

/*
    @brief Logs one line through chatwire's logger.
    @details
    Exists so dllmain — which cannot import the module — can still report why it
    gave up, instead of failing silently.
*/
extern "C" auto chatwire_log(const char* const message) -> void
{
    if (message != nullptr) { chatwire::log::info("{}", message); }
}
