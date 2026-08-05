// chatwire — a live WebSocket API for Minecraft 1.8.9.
//
// This is the root module: the only thing a host has to import, and the only
// place the pieces are wired to each other.
//
// ===========================================================================
// STARTUP ORDER, WHICH IS NOT ARBITRARY
// ===========================================================================
//   1. wait for a JVM that looks like Minecraft, and detect the mapping mode
//   2. register the SDK wrappers under that mode's class names
//   3. install the PUMP hook (Minecraft.runTick) — everything else needs a way
//      onto the game thread, so this comes before any feature
//   4. start the features, ON the game thread via the pump
//   5. start the WebSocket server last, so a client that connects instantly
//      finds a working API rather than a half-built one
//
// Shutdown is the exact reverse, and for the same reasons.
module;

// The shared preamble, FIRST and identical in every module.  See the header
// for why GCC 15 requires that of a modular build.
#include "core/prelude.hpp"

export module chatwire;

// Re-exported: a host that imports chatwire gets the public surface — logging,
// the feature interface, the JSON helpers, the pump and the mapping tables —
// without having to know which submodule each name lives in.
export import chatwire.core.feature;
export import chatwire.core.json;
export import chatwire.core.log;
export import chatwire.core.pump;
export import chatwire.mapping;

// NOT re-exported.  chatwire.sdk is the module that includes vmhook.hpp, and
// chatwire.ws.server is the one that includes <winsock2.h>.  Re-exporting either
// would drag those 24k lines (and Winsock's extern "C" block) into every
// importer's translation unit, which is what today's compilers cannot survive —
// see the header of src/sdk.ixx.  They are implementation detail regardless: a
// host talks to chatwire through start()/stop() and the feature interface, never
// through vmhook or a socket.
//
// They are imported by the implementation unit, which is the only TU that needs
// them.

export namespace chatwire
{
    /* @brief chatwire's own version. */
    inline constexpr std::string_view version{ "0.1.0" };

    /* @brief The default port.  Loopback only; see ws/server.ixx. */
    inline constexpr std::uint16_t default_port{ 24455 };

    /*
        @brief Brings chatwire up.  Call from an injected thread, not DllMain.
        @details
        Blocks until the JVM is ready (bounded by `timeout`), then installs
        everything.  Safe to call twice; the second call is a no-op.

        DECLARED here and DEFINED in chatwire_impl.cpp, deliberately.  An inline
        definition in a module interface is emitted into every importer, and GCC
        15 segfaults compiling this one there.  Keeping the body in a single
        implementation unit is also the ordinary shape for a function this size:
        importers get a call, not a copy.

        @param port     TCP port on 127.0.0.1.
        @param timeout  How long to wait for Minecraft's classes to appear.  A
                        client still on the launcher screen has a JVM but no
                        Minecraft class yet.
        @return false when no supported Minecraft was found or the pump could not
                be installed — in which case nothing was left installed.
    */
    [[nodiscard]] auto start(std::uint16_t port = default_port,
                             std::chrono::seconds timeout = std::chrono::seconds{ 120 }) noexcept
        -> bool;

    /*
        @brief Takes chatwire down.  Safe to call more than once.
        @details
        Reverse of start(), and the order is what keeps unload from crashing:
        server first (no new work can arrive), then the chat sink, then the
        features (hooks come down on the game thread), then the pump.  Every
        stage is bounded; nothing here can hang the game.
    */
    auto stop() noexcept -> void;

    /* @brief How many WebSocket clients are connected. */
    [[nodiscard]] auto client_count() noexcept -> std::size_t;

    [[nodiscard]] auto is_running() noexcept -> bool;
}
