// chatwire.features.system — chatwire talking about itself.
//
// Everything here is about the bridge rather than about the game: what version
// is running, who is connected, how much has gone through it, and how to make
// it let go.
//
// It is therefore the one feature whose commands are NOT named after Minecraft
// members, and the only one that keeps a short prefix.  Every other command
// spells out the Java member it reaches, because that name can be checked
// against the game's source; `system.status` reaches nothing in the game, so
// there is nothing to spell out and nothing to check.
//
// It is also the second feature, which is the point at which "adding one is a
// new file and two lines" stops being a claim and starts being a fact.  Nothing
// in the server, the dispatcher or the protocol changed to make `system.*` work.
#pragma once

#include "chatwire/common.hpp"

// The root header, for chatwire::version.  Not a layering violation and not a
// cycle: chatwire.hpp is the PUBLIC surface and includes no feature, so a
// feature reading a constant out of it depends on nothing that depends back.
#include "chatwire/chatwire.hpp"
#include "chatwire/feature.hpp"
#include "chatwire/json.hpp"
#include "chatwire/log.hpp"
#include "chatwire/mapping.hpp"

namespace chatwire::features
{
    /*
        @brief What `system.detach` calls to unload chatwire.
        @details
        A plain function pointer installed by the host (dllmain), for the same
        reason the console's is: the library knows how to STOP, but only the
        module knows how to unload itself, and only the module has the handle to
        do it with.
    */
    using detach_request = void (*)();

    inline std::atomic<detach_request> g_detach{ nullptr };

    /* @brief How many clients are connected.  Supplied by the host. */
    using client_counter = std::size_t (*)() noexcept;

    /*
        @brief Where `system.stats` gets its numbers.
        @details
        Returns a ready-made JSON object.  The counters belong to whichever
        features keep them -- the chat feature's lines/sent/added, the commands
        feature's run/dropped -- and `system` is where they are ANSWERED,
        because they are chatwire's own bookkeeping rather than anything the game
        has a name for.  A function pointer the host installs keeps that from
        becoming an include from one feature into another, and keeps the JOINING
        of several features' counters out of here too: `system` reports one
        object and does not need to know how many places it came from.
    */
    using stats_source = std::string (*)();

    inline std::atomic<stats_source> g_stats{ nullptr };

    /* Filled in by chatwire::start() so status has something to report. */
    inline std::atomic<std::uint16_t> g_status_port{ 0 };
    /* Whether vmhook will let this JVM be called into at all. */
    inline std::atomic<bool> g_can_call{ false };
    inline std::atomic<client_counter> g_client_counter{ nullptr };

    /*
        @brief What `system.*` answers with.
        @details
        `stats` is not among these: its shape is every counter-keeping feature's
        struct flattened together, and which features those are is the host's
        business rather than this one's -- see stats_source above.

        `port` and `clients` keep the types they are counted in.  They used to be
        cast to std::int64_t at the call site, not because anything wanted them
        widened but because there was one numeric overload of json::field and
        that was its parameter.  The writer takes a member's own type now, so
        the cast is gone and a port is a std::uint16_t all the way to the wire.
    */
    struct pong_result   { bool pong{ true }; };
    struct detaching_result { bool detaching{ true }; };
    struct status_result
    {
        std::string_view version{};
        std::string_view mapping{};
        std::uint16_t    port{ 0 };
        std::size_t      clients{ 0 };
        bool             can_call{ false };
        /*
            Which features are up.  Not decoration: a feature whose class was
            not loaded when chatwire arrived starts LATER, and until it does the
            thing it provides silently does not happen.  `chat` at the main menu
            is the normal case -- GuiNewChat does not exist until a chat box has
            been drawn -- so a client that wants chat should wait for this to say
            so rather than assume.
        */
        std::vector<chatwire::registry::status_line> features{};
    };

    class system_feature final : public chatwire::feature
    {
    public:
        [[nodiscard]] auto name() const noexcept -> std::string_view override
        {
            return "system";
        }

        /* Nothing to install: this feature reads state, it does not hook. */
        [[nodiscard]] auto start() noexcept -> bool override { return true; }
        auto stop() noexcept -> void override { }

        [[nodiscard]] auto handle(const chatwire::command& cmd) noexcept
            -> chatwire::response override
        {
            try
            {
                if (cmd.verb == "ping")
                {
                    return chatwire::response::success(
                        chatwire::json::object(pong_result{}));
                }

                if (cmd.verb == "status")
                {
                    const client_counter clients{ g_client_counter.load(std::memory_order_acquire) };
                    return chatwire::response::success(
                        chatwire::json::object(status_result{
                            .version  = chatwire::version,
                            .mapping  = chatwire::mapping::mode_name(chatwire::mapping::current),
                            .port     = g_status_port.load(std::memory_order_relaxed),
                            .clients  = clients ? clients() : 0u,
                            .can_call = g_can_call.load(std::memory_order_relaxed),
                            .features = chatwire::registry::status() }));
                }

                if (cmd.verb == "stats")
                {
                    const stats_source source{ g_stats.load(std::memory_order_acquire) };
                    if (!source)
                    {
                        return chatwire::response::failure(
                            "no stats source is installed in this build");
                    }
                    return chatwire::response::success(source());
                }

                if (cmd.verb == "detach")
                {
                    const detach_request request{ g_detach.load(std::memory_order_acquire) };
                    if (!request)
                    {
                        return chatwire::response::failure(
                            "this build has no detach handler installed");
                    }

                    // The teardown cannot run on THIS thread: chatwire::stop()
                    // joins every client thread, and the thread asking to detach
                    // is one of them -- it would join itself and deadlock.  So
                    // the handler starts a thread of its own and returns at once.
                    //
                    // What it must NOT do is start a thread HERE that outlives
                    // the call.  The unload ends in FreeLibraryAndExitThread,
                    // which makes the unload safe for the thread CALLING it and
                    // for no other: any thread still executing code in this
                    // module when it runs -- a lambda body, std::thread's
                    // trampoline, the CRT's thread-exit path, all of which live
                    // in the DLL -- is running on freed pages, and dies at some
                    // unpredictable later moment.  An earlier version spawned a
                    // detached std::thread here that slept and then called
                    // request(); it crashed the game minutes after a detach that
                    // had appeared to work.
                    //
                    // The delay that lets this reply reach the client now lives
                    // at the top of the detach worker instead, where the thread
                    // waiting it out is the same one that unloads.
                    request();

                    chatwire::log::warn("detach requested over the websocket");
                    return chatwire::response::success(
                        chatwire::json::object(detaching_result{}));
                }

                return chatwire::response::failure(
                    "unknown verb; try status, stats, ping or detach");
            }
            catch (...)
            {
                return chatwire::response::failure("internal error");
            }
        }
    };
}

namespace chatwire::features::system
{
    /* @brief Installs what `system.detach` should call.  See detach_request. */
    inline auto set_detach_handler(const chatwire::features::detach_request request) noexcept
        -> void
    {
        chatwire::features::g_detach.store(request, std::memory_order_release);
    }

    /* @brief Tells `system.status` which port ended up bound. */
    inline auto set_status_port(const std::uint16_t port) noexcept -> void
    {
        chatwire::features::g_status_port.store(port, std::memory_order_release);
    }

    /* @brief Tells `system.status` whether calls into the game are possible. */
    inline auto set_can_call(const bool can_call) noexcept -> void
    {
        chatwire::features::g_can_call.store(can_call, std::memory_order_release);
    }

    /* @brief Tells `system.stats` where to get its counters.  See stats_source. */
    inline auto set_stats_source(const chatwire::features::stats_source source) noexcept -> void
    {
        chatwire::features::g_stats.store(source, std::memory_order_release);
    }

    /* @brief Tells `system.status` where to ask how many clients are connected. */
    inline auto set_client_counter(const chatwire::features::client_counter counter) noexcept
        -> void
    {
        chatwire::features::g_client_counter.store(counter, std::memory_order_release);
    }

    /* @brief This feature's singleton, for the root module to register. */
    [[nodiscard]] inline auto instance() noexcept -> chatwire::feature*
    {
        static chatwire::features::system_feature feature{};
        return &feature;
    }
}
