// chatwire.features.system — chatwire talking about itself.
//
// Everything here is about the bridge rather than about the game: what version
// is running, who is connected, and how to make it let go.
//
// It is also the second feature, which is the point at which "adding one is a
// new file and two lines" stops being a claim and starts being a fact.  Nothing
// in the server, the dispatcher or the protocol changed to make `system.*` work.
#pragma once

#include "chatwire/common.hpp"
#include "chatwire/feature.hpp"
#include "chatwire/json.hpp"
#include "chatwire/log.hpp"
#include "chatwire/mapping.hpp"
#include "chatwire/pump.hpp"

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

    /* Filled in by chatwire::start() so status has something to report. */
    inline std::atomic<std::uint16_t> g_status_port{ 0 };
    inline std::atomic<std::size_t> (*g_client_counter)() { nullptr };

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
                        chatwire::json::object(chatwire::json::field("pong", true)));
                }

                if (cmd.verb == "status")
                {
                    const auto pump{ chatwire::pump::snapshot() };
                    return chatwire::response::success(chatwire::json::object(
                        chatwire::json::field("mapping",
                            chatwire::mapping::mode_name(chatwire::mapping::current))
                        + "," + chatwire::json::field("port",
                            static_cast<std::int64_t>(g_status_port.load(std::memory_order_relaxed)))
                        + "," + chatwire::json::field("queued",
                            static_cast<std::int64_t>(pump.pending))
                        + "," + chatwire::json::field("tasks_run",
                            static_cast<std::int64_t>(pump.executed))
                        + "," + chatwire::json::field("tasks_dropped",
                            static_cast<std::int64_t>(pump.dropped))
                        + "," + chatwire::json::field("tasks_failed",
                            static_cast<std::int64_t>(pump.failed))));
                }

                if (cmd.verb == "detach")
                {
                    const detach_request request{ g_detach.load(std::memory_order_acquire) };
                    if (!request)
                    {
                        return chatwire::response::failure(
                            "this build has no detach handler installed");
                    }

                    // The reply has to reach the client BEFORE the server that
                    // would deliver it is torn down, and the teardown cannot run
                    // on THIS thread at all: chatwire::stop() joins every client
                    // thread, and the thread asking to detach is one of them --
                    // it would join itself and deadlock.
                    //
                    // So: hand the request to a detached thread that pauses long
                    // enough for this response to be written, then unloads.  The
                    // pause is generous rather than precise; there is nothing
                    // waiting on it.
                    try
                    {
                        std::thread{ [request]() noexcept
                        {
                            std::this_thread::sleep_for(std::chrono::milliseconds{ 300 });
                            request();
                        } }.detach();
                    }
                    catch (...)
                    {
                        return chatwire::response::failure(
                            "could not start the detach thread");
                    }

                    chatwire::log::warn("detach requested over the websocket");
                    return chatwire::response::success(chatwire::json::object(
                        chatwire::json::field("detaching", true)));
                }

                return chatwire::response::failure(
                    "unknown verb; try status, ping or detach");
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

    /* @brief This feature's singleton, for the root module to register. */
    [[nodiscard]] inline auto instance() noexcept -> chatwire::feature*
    {
        static chatwire::features::system_feature feature{};
        return &feature;
    }
}
