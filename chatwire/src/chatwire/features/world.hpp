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
//
// ===========================================================================
// AND WHAT IT PUSHES
// ===========================================================================
//   net.minecraft.client.Minecraft.loadWorld
//                        -> the client changed world.  `loaded` false means it
//                           LEFT one: a disconnect, or a return to the title
//                           screen.
//
// This is why the feature hooks at all now, and it is the event that makes
// `playerEntities` usable without polling: a client that wants the roster of
// wherever the player currently is can ask once, when told the world changed,
// instead of asking every second and comparing.
//
// It is also the only POSITIVE report of a disconnect chatwire has.  Everything
// else — `in_world()`, "not in a world" on a chat command — is the absence of a
// player, which a client can only discover by asking at the right moment.
#include "chatwire/common.hpp"

#include "chatwire/feature.hpp"
#include "chatwire/json.hpp"
#include "chatwire/log.hpp"
#include "chatwire/mapping.hpp"
#include "chatwire/sdk.hpp"

namespace chatwire::features
{
    inline std::atomic<std::uint64_t> g_player_queries{ 0 };

    /*
        @brief Where a world change goes once the detour has seen it.
        @details
        The same shape as the chat feature's sink, and for the same reason: the
        server owns the broadcast, and a feature that knew about sockets could
        not be tested without one.  A plain function pointer, installed once by
        the host before start(), read atomically from inside a detour.
    */
    using world_sink = void (*)(std::string_view json_line) noexcept;

    inline std::atomic<world_sink>    g_world_sink{ nullptr };
    inline std::atomic<std::uint64_t> g_worlds_entered{ 0 };
    inline std::atomic<std::uint64_t> g_worlds_left{ 0 };

    class world_feature final : public chatwire::feature
    {
    public:
        [[nodiscard]] auto name() const noexcept -> std::string_view override
        {
            return "world";
        }

        /*
            @brief Answers to the Java classes it reads and hooks, and to
                   nothing else.
            @details
            `net.minecraft.world.World.playerEntities` is the whole command.  The
            length is not decoration: it names the exact field, so a client
            author can check what they are getting against Minecraft's source
            without trusting this file's summary of it.  `world` used to be
            accepted as a short prefix and no longer is -- see the note at the
            top of this file for why that one is worth losing.

            Minecraft is claimed for the same reason the chat feature claims
            GuiNewChat: `loadWorld` is the method this feature hooks, so the
            event it pushes and a future command for it name the same class.
        */
        [[nodiscard]] auto claims(const std::string_view prefix) const noexcept -> bool override
        {
            return prefix == "net.minecraft.world.World"
                || prefix == "net.minecraft.client.Minecraft";
        }

        /*
            @brief Installs the loadWorld observer.
            @details
            Failing is reported but is not much of a failure: `playerEntities`
            reads the world and does not care whether anything is watching it,
            so a build where the hook will not install still answers every
            command this feature has.  What is lost is the push, and a client
            that wanted it would otherwise poll -- so it is worth a line in the
            log saying which one it got.  Same shape as the chat feature, whose
            observer can fail while sending still works.
        */
        [[nodiscard]] auto start() noexcept -> bool override
        {
            if (!chatwire::sdk::install_world_observer(&on_world_change))
            {
                chatwire::log::warn("world: could not observe world changes; "
                                    "playerEntities still works");
                return false;
            }
            return true;
        }

        auto stop() noexcept -> void override
        {
            // Hooks come down centrally, in chatwire::stop() -> sdk::remove_hooks(),
            // after every thread that could be inside one has been joined.  Same
            // as the chat feature: all a feature has to do is stop producing.
        }

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
                        entries += chatwire::json::object(std::format("{},{}",
                            chatwire::json::field("name", who.name),
                            chatwire::json::field("uuid", who.uuid)));
                    }

                    return chatwire::response::success(
                        chatwire::json::object(std::format("{},\"players\":[{}]",
                            chatwire::json::field("count",
                                static_cast<std::int64_t>(found.size())),
                            entries)));
                }

                return chatwire::response::failure(
                    "unknown member; try playerEntities");
            }
            catch (...)
            {
                return chatwire::response::failure("internal error");
            }
        }

    private:
        /*
            @brief Receives one world change, inside the loadWorld detour.
            @details
            `loaded` false is the client LEAVING a world, which is the half a
            client cannot easily see any other way.

            Nothing is read out of the game here, and that is deliberate rather
            than lazy.  This runs BEFORE loadWorld's body: on the way in the new
            world is not installed yet, and on the way out the old player is
            already being torn down, so anything this asked about would describe
            neither the world being left nor the one being entered.  A client
            that wants the new roster asks for `playerEntities` when it gets
            this, by which time the world is real.

            noexcept and fully guarded: the frame above is Minecraft's
            interpreter, which has no handler for a C++ exception.
        */
        static auto on_world_change(const bool loaded) noexcept -> void
        {
            try
            {
                (loaded ? g_worlds_entered : g_worlds_left)
                    .fetch_add(1, std::memory_order_relaxed);

                const world_sink sink{ g_world_sink.load(std::memory_order_acquire) };
                if (!sink) { return; }   // nobody watching; do no work

                // Named for the method it comes out of, in full, exactly as the
                // chat event is -- so a client reads `type` the same way whoever
                // wrote it read Minecraft's source.
                const std::string payload{ chatwire::json::object(std::format("{},{}",
                    chatwire::json::field(
                        "type", "net.minecraft.client.Minecraft.loadWorld"),
                    chatwire::json::field("loaded", loaded))) };

                sink(payload);
            }
            catch (...)
            {
                // Never let anything reach the interpreter frame above.
            }
        }
    };
}

namespace chatwire::features::world
{
    /* @brief Installs the sink every observed world change is handed to. */
    inline auto set_sink(const chatwire::features::world_sink sink) noexcept -> void
    {
        chatwire::features::g_world_sink.store(sink, std::memory_order_release);
    }

    /*
        @brief This feature's counters, as JSON fields WITHOUT the braces.
        @details
        A fragment, joined with the other features' by the host and wrapped
        once -- see the same function in features/chat.hpp for why the shape is
        this and not a finished object.  `player_queries` counted from the day
        this feature existed and had nowhere to be reported until there was a
        second number here to report it with.
    */
    [[nodiscard]] inline auto stats_json() -> std::string
    {
        return std::format("{},{},{}",
            chatwire::json::field("player_queries",
                static_cast<std::int64_t>(
                    chatwire::features::g_player_queries.load(std::memory_order_relaxed))),
            chatwire::json::field("worlds_entered",
                static_cast<std::int64_t>(
                    chatwire::features::g_worlds_entered.load(std::memory_order_relaxed))),
            chatwire::json::field("worlds_left",
                static_cast<std::int64_t>(
                    chatwire::features::g_worlds_left.load(std::memory_order_relaxed))));
    }

    /* @brief This feature's singleton, for the root module to register. */
    [[nodiscard]] inline auto instance() noexcept -> chatwire::feature*
    {
        static chatwire::features::world_feature feature{};
        return &feature;
    }
}
