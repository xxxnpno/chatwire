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
    /* How many times a client asked what the local player is. */
    inline std::atomic<std::uint64_t> g_player_states{ 0 };
    /* How many times a client asked for every loaded entity. */
    inline std::atomic<std::uint64_t> g_entity_queries{ 0 };

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

    /*
        @brief The client changed world.  `loaded` false is it LEAVING one.
        @details
        The event's name is a default member initialiser rather than something
        the one construction site spells, for the same reason the chat event's
        is: it is a property of the event.
    */
    struct load_world_event
    {
        std::string_view type{ "net.minecraft.client.Minecraft.loadWorld" };
        bool             loaded{ false };
    };

    /*
        @brief The answer to `playerEntities`.
        @details
        `players` is a vector of chatwire::sdk::player_identity -- the sdk's OWN
        type, put on the wire unchanged, because its members are already spelled
        `name` and `uuid`.  There is no conversion step and no second struct
        mirroring the first: json::object writes an array of objects by walking
        whatever the element type turns out to be.

        What that replaced was a loop that built each entry's JSON by hand and
        pasted the results into a format string with the array's brackets typed
        into it as literal text -- `"{},\"players\":[{}]"` -- which is the point
        where a wire format stops being described anywhere and starts being
        assembled.
    */
    struct player_entities_result
    {
        std::size_t                              count{ 0 };
        std::vector<chatwire::sdk::player_identity> players{};
    };

    /*
        @brief The answer to `thePlayer` — everything that object is right now.
        @details
        One command rather than eleven, and named after the FIELD it reads, for
        the same reason `playerEntities` is: `Minecraft.thePlayer` is a real
        member a reader can check, and what it returns is what that object is.
        Eleven commands named after eleven Entity fields would be eleven round
        trips to describe one instant, and the instant is the point -- position
        and health read a tick apart describe a player who never existed.

        `dimension` is Minecraft's number: 0 overworld, -1 nether, 1 end.
        `health` is out of 20 and `food` out of 20, both as the HUD shows them.
    */
    struct the_player_result
    {
        std::string  name{};
        double       x{ 0.0 };
        double       y{ 0.0 };
        double       z{ 0.0 };
        float        yaw{ 0.0F };
        float        pitch{ 0.0F };
        bool         on_ground{ false };
        std::int32_t dimension{ 0 };
        float        health{ 0.0F };
        std::int32_t food{ 0 };
        float        saturation{ 0.0F };
        std::int32_t experience_level{ 0 };
    };

    /*
        @brief The answer to `currentScreen`.
        @details
        `open` is false in the world, which is when `screen` is "".  Otherwise
        `screen` is the class the screen REALLY is, in the attached client's own
        spelling -- `net/minecraft/client/gui/GuiChat` on a deobfuscated client,
        `axz` on a vanilla one.  Both are the honest answer for that jar, and a
        client that wants to compare them can ask `mapping.resolve`.
    */
    struct current_screen_result
    {
        bool        open{ false };
        std::string screen{};
    };

    /*
        @brief The answer to `loadedEntityList`.
        @details
        EVERYTHING the client has loaded, not just players: mobs, dropped items,
        arrows, paintings.  `type` on each one is the class it really is, so a
        caller filters on that rather than chatwire having to know what a zombie
        is -- and the answer is honest on a vanilla client, where the class is
        called `zj`.
    */
    struct loaded_entities_result
    {
        std::size_t                              count{ 0 };
        std::vector<chatwire::sdk::entity_view>  entities{};
    };

    /* @brief This feature's contribution to `system.stats`. */
    struct world_stats
    {
        std::uint64_t player_queries{ 0 };
        std::uint64_t worlds_entered{ 0 };
        std::uint64_t worlds_left{ 0 };
        std::uint64_t player_states{ 0 };
        std::uint64_t entity_queries{ 0 };
    };

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

                    auto found{ chatwire::sdk::players() };
                    g_player_queries.fetch_add(1, std::memory_order_relaxed);

                    const std::size_t count{ found.size() };
                    return chatwire::response::success(
                        chatwire::json::object(player_entities_result{
                            .count = count, .players = std::move(found) }));
                }

                if (cmd.verb == "thePlayer")
                {
                    const auto state{ chatwire::sdk::local_player_state() };
                    if (!state)
                    {
                        return chatwire::response::failure("not in a world");
                    }
                    g_player_states.fetch_add(1, std::memory_order_relaxed);

                    return chatwire::response::success(
                        chatwire::json::object(the_player_result{
                            .name             = state->name,
                            .x                = state->x,
                            .y                = state->y,
                            .z                = state->z,
                            .yaw              = state->yaw,
                            .pitch            = state->pitch,
                            .on_ground        = state->on_ground,
                            .dimension        = state->dimension,
                            .health           = state->health,
                            .food             = state->food,
                            .saturation       = state->saturation,
                            .experience_level = state->experience_level }));
                }

                if (cmd.verb == "currentScreen")
                {
                    // Answered in or out of a world: "no screen is open" is a
                    // real answer at the title screen too, and a client polling
                    // this to know when it may type should not have to special
                    // case the one moment it most wants to watch.
                    auto screen{ chatwire::sdk::current_screen_class() };
                    const bool open{ !screen.empty() };
                    return chatwire::response::success(
                        chatwire::json::object(current_screen_result{
                            .open = open, .screen = std::move(screen) }));
                }

                if (cmd.verb == "loadedEntityList")
                {
                    if (!chatwire::sdk::in_world())
                    {
                        return chatwire::response::failure("not in a world");
                    }
                    auto found{ chatwire::sdk::entities() };
                    g_entity_queries.fetch_add(1, std::memory_order_relaxed);
                    const std::size_t count{ found.size() };
                    return chatwire::response::success(
                        chatwire::json::object(loaded_entities_result{
                            .count = count, .entities = std::move(found) }));
                }

                return chatwire::response::failure(
                    "unknown member; try playerEntities, loadedEntityList, thePlayer "
                    "or currentScreen");
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

                const std::string payload{
                    chatwire::json::object(load_world_event{ .loaded = loaded }) };

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
        @brief This feature's counters, for `system.stats`.
        @details
        One of several structs the host flattens into a single object -- see the
        same function in features/chat.hpp.  `player_queries` counted from the
        day this feature existed and had nowhere to be reported until there was
        a second number here to report it with.
    */
    [[nodiscard]] inline auto stats() noexcept -> chatwire::features::world_stats
    {
        return chatwire::features::world_stats{
            .player_queries = chatwire::features::g_player_queries.load(std::memory_order_relaxed),
            .worlds_entered = chatwire::features::g_worlds_entered.load(std::memory_order_relaxed),
            .worlds_left    = chatwire::features::g_worlds_left.load(std::memory_order_relaxed),
            .player_states  = chatwire::features::g_player_states.load(std::memory_order_relaxed),
            .entity_queries = chatwire::features::g_entity_queries.load(std::memory_order_relaxed) };
    }

    /* @brief This feature's singleton, for the root module to register. */
    [[nodiscard]] inline auto instance() noexcept -> chatwire::feature*
    {
        static chatwire::features::world_feature feature{};
        return &feature;
    }
}
