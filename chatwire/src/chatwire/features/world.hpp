#pragma once

// chatwire.features.world — what the client can see of the world.
//
// The third feature, and the one that makes the extension-point claim concrete:
// nothing in the server, the dispatcher, the protocol or the injection path
// changed to add it.  A new file, and one `registry::add` in src/chatwire.cpp.
//
// ===========================================================================
// WHAT IT DOES
// ===========================================================================
//   net.minecraft.world.World.playerEntities
//                        -> every player the client currently has loaded, with
//                           the name and the UUID of each.
//
// The command IS the field it reads, and that is a promise about the answer
// rather than a naming style.  A command called `players` or `world.list` would
// suggest the server's roster; this is the client's entity list, which is the
// players close enough to exist as entities.  On a large server that is a small
// fraction of the tab list.  Naming it after the field means the answer cannot
// be mistaken for something it is not, and a reader who knows Minecraft's source
// already knows the difference.
//
// Which is exactly why the short spellings `world.playerEntities` and
// `world.players` are gone rather than kept as conveniences: the short name is
// the misleading one, and leaving it available meant most callers would type it.
//
// Name and UUID come from the SAME object in the same pass, so an entry's two
// halves always belong together — which is not true if a caller has to ask for
// them separately and the world moves in between.
#include "chatwire/common.hpp"

#include "chatwire/feature.hpp"
#include "chatwire/json.hpp"
#include "chatwire/log.hpp"
#include "chatwire/mapping.hpp"
#include "chatwire/sdk.hpp"

namespace chatwire::features
{
    inline std::atomic<std::uint64_t> g_player_queries{ 0 };

    class world_feature final : public chatwire::feature
    {
    public:
        [[nodiscard]] auto name() const noexcept -> std::string_view override
        {
            return "world";
        }

        /*
            @brief Answers to the Java class it reads, and to nothing else.
            @details
            `net.minecraft.world.World.playerEntities` is the whole command.  The
            length is not decoration: it names the exact field, so a client
            author can check what they are getting against Minecraft's source
            without trusting this file's summary of it.  `world` used to be
            accepted as a short prefix and no longer is -- see the note at the
            top of this file for why that one is worth losing.
        */
        [[nodiscard]] auto claims(const std::string_view prefix) const noexcept -> bool override
        {
            return prefix == "net.minecraft.world.World";
        }

        /* Nothing to install: this feature reads, it does not hook. */
        [[nodiscard]] auto start() noexcept -> bool override { return true; }
        auto stop() noexcept -> void override { }

        [[nodiscard]] auto handle(const chatwire::command& cmd) noexcept
            -> chatwire::response override
        {
            try
            {
                if (cmd.verb == "playerEntities")
                {
                    if (!chatwire::sdk::in_world())
                    {
                        return chatwire::response::failure("not in a world");
                    }

                    const auto found{ chatwire::sdk::players() };
                    g_player_queries.fetch_add(1, std::memory_order_relaxed);

                    std::string entries;
                    for (const auto& who : found)
                    {
                        if (!entries.empty()) { entries += ","; }
                        entries += chatwire::json::object(
                            chatwire::json::field("name", who.name) + ","
                            + chatwire::json::field("uuid", who.uuid));
                    }

                    return chatwire::response::success(chatwire::json::object(
                        chatwire::json::field("count",
                            static_cast<std::int64_t>(found.size()))
                        + ",\"players\":[" + entries + "]"));
                }

                return chatwire::response::failure(
                    "unknown member; try playerEntities");
            }
            catch (...)
            {
                return chatwire::response::failure("internal error");
            }
        }
    };
}

namespace chatwire::features::world
{
    /* @brief This feature's singleton, for the root module to register. */
    [[nodiscard]] inline auto instance() noexcept -> chatwire::feature*
    {
        static chatwire::features::world_feature feature{};
        return &feature;
    }
}
