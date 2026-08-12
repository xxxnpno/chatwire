#pragma once

// chatwire.features.scoreboard — the sidebar, the teams and the tab list.
//
// ===========================================================================
// THREE LISTS THAT ARE CONSTANTLY CONFUSED
// ===========================================================================
// A server shows a player's name in at least three places, and they are not the
// same list, do not update together, and can disagree:
//
//   net.minecraft.world.World.playerEntities
//                   who the CLIENT has loaded as entities -- the players near
//                   enough to exist.  On a big server, a small fraction.
//   NetHandlerPlayClient.getPlayerInfoMap
//                   who the SERVER says is connected.  The tab list.  Everyone,
//                   including players in another world.
//   Scoreboard.getTeams
//                   how names are DECORATED.  A team's prefix and suffix are
//                   what make one nametag red and another blue, and a team can
//                   list a name that belongs to nobody online.
//
// Each command here is the member it reads, so which list you asked for is
// written into the question.  That is not a naming style: "get the players" is
// the ambiguity this API exists to remove.
//
// ===========================================================================
// IT IS THE CLIENT'S COPY, ALWAYS
// ===========================================================================
// Every one of these is fed by the server's packets, so it holds exactly what
// this player has been told.  An objective the server hides from you is hidden
// from this; a team it has not sent does not exist here.  That is a property of
// Minecraft rather than a limitation of the bridge, and naming the commands
// after the getters is what keeps it obvious.
//
// ===========================================================================
// COST
// ===========================================================================
// These are the most expensive reads chatwire has: a scoreboard is an object, a
// collection, and a string call per row, all inside one java_thread_scope --
// which the whole VM waits on.  Every one is bounded (256 scores, 128 teams,
// 512 tab rows) and every one is a REQUEST.  Nothing here should be polled at
// tick rate; ask when something happened, which is what the chat and world
// events are for.
#include "chatwire/common.hpp"

#include "chatwire/feature.hpp"
#include "chatwire/json.hpp"
#include "chatwire/log.hpp"
#include "chatwire/mapping.hpp"
#include "chatwire/sdk.hpp"

namespace chatwire::features
{
    inline std::atomic<std::uint64_t> g_scoreboard_queries{ 0 };
    inline std::atomic<std::uint64_t> g_team_queries{ 0 };
    inline std::atomic<std::uint64_t> g_tab_queries{ 0 };

    /*
        @brief The answer to `getTeams`.
        @details
        `count` is spelled out rather than left to the array's length because
        every other list command here carries one, and a client that only wants
        to know whether anything changed should not have to parse the whole
        array to find out.
    */
    struct teams_result
    {
        std::size_t                             count{ 0 };
        std::vector<chatwire::sdk::team_view>   teams{};
    };

    /* @brief The answer to `getPlayerInfoMap` — the tab list. */
    struct tab_list_result
    {
        std::size_t                             count{ 0 };
        std::vector<chatwire::sdk::tab_entry>   players{};
    };

    /* @brief This feature's contribution to `system.stats`. */
    struct scoreboard_stats
    {
        std::uint64_t scoreboard_queries{ 0 };
        std::uint64_t team_queries{ 0 };
        std::uint64_t tab_queries{ 0 };
    };

    class scoreboard_feature final : public chatwire::feature
    {
    public:
        [[nodiscard]] auto name() const noexcept -> std::string_view override
        {
            return "scoreboard";
        }

        /*
            @brief Answers to the two classes it reads, and nothing else.
            @details
            The tab list lives on NetHandlerPlayClient rather than on the
            scoreboard, and the command says so.  Putting it under a short
            `tab.*` would have been friendlier to type and would have hidden the
            one fact a caller most needs: it comes from the CONNECTION, so it is
            empty before a world is joined and it is the server's list rather
            than the client's.
        */
        [[nodiscard]] auto claims(const std::string_view prefix) const noexcept -> bool override
        {
            return prefix == "net.minecraft.scoreboard.Scoreboard"
                || prefix == "net.minecraft.client.network.NetHandlerPlayClient";
        }

        /*
            @brief Nothing to install.
            @details
            This feature reads and never hooks, so it has nothing that can fail
            to start and nothing to retry.  It answers "not in a world" until
            there is one, which is a per-request answer rather than a state.
        */
        [[nodiscard]] auto start() noexcept -> bool override { return true; }
        auto stop() noexcept -> void override { }

        [[nodiscard]] auto handle(const chatwire::command& cmd) noexcept
            -> chatwire::response override
        {
            try
            {
                if (cmd.verb == "getObjectiveInDisplaySlot") { return this->objective(cmd); }
                if (cmd.verb == "getTeams")                  { return this->all_teams(); }
                if (cmd.verb == "getPlayersTeam")            { return this->players_team(cmd); }
                if (cmd.verb == "getPlayerInfoMap")          { return this->tab(); }

                return chatwire::response::failure(
                    "unknown member; try getObjectiveInDisplaySlot, getTeams, "
                    "getPlayersTeam or getPlayerInfoMap");
            }
            catch (...)
            {
                return chatwire::response::failure("internal error");
            }
        }

    private:
        /*
            @brief `getObjectiveInDisplaySlot(slot)`.
            @details
            The slot is Minecraft's own number, and the reply names it back in
            words so a reader of a log does not have to remember which is which:
            0 list, 1 sidebar, 2 belowName.  `"sidebar"` is accepted as well,
            because a client author writing this by hand should not have to look
            the number up.
        */
        [[nodiscard]] static auto objective(const chatwire::command& cmd) -> chatwire::response
        {
            std::int32_t slot{ 1 };                       // the sidebar, by far the usual one
            if (const auto text{ chatwire::json::get_string(cmd.body, "slot") })
            {
                if (*text == "list")           { slot = 0; }
                else if (*text == "sidebar")   { slot = 1; }
                else if (*text == "belowName") { slot = 2; }
                else if (text->size() == 1u && *text >= "0" && *text <= "2")
                {
                    slot = text->front() - '0';
                }
                else
                {
                    return chatwire::response::failure(
                        "'slot' must be list, sidebar, belowName, or 0-2");
                }
            }

            auto view{ chatwire::sdk::scoreboard_slot(slot) };
            if (!view) { return chatwire::response::failure("not in a world"); }
            g_scoreboard_queries.fetch_add(1, std::memory_order_relaxed);
            return chatwire::response::success(chatwire::json::object(*view));
        }

        [[nodiscard]] static auto all_teams() -> chatwire::response
        {
            if (!chatwire::sdk::in_world())
            {
                return chatwire::response::failure("not in a world");
            }
            auto found{ chatwire::sdk::teams() };
            g_team_queries.fetch_add(1, std::memory_order_relaxed);
            const std::size_t count{ found.size() };
            return chatwire::response::success(chatwire::json::object(
                teams_result{ .count = count, .teams = std::move(found) }));
        }

        /*
            @brief `getPlayersTeam(name)`.
            @details
            Fails with a plain "on no team" rather than an empty object: a client
            asking this wants to colour a name, and "there is nothing to colour"
            is a different answer from "a team with no prefix".
        */
        [[nodiscard]] static auto players_team(const chatwire::command& cmd) -> chatwire::response
        {
            const auto who{ chatwire::json::get_string(cmd.body, "name") };
            if (!who || who->empty())
            {
                return chatwire::response::failure("missing or non-string 'name'");
            }
            auto found{ chatwire::sdk::team_of(*who) };
            if (!found)
            {
                return chatwire::response::failure("that name is on no team");
            }
            g_team_queries.fetch_add(1, std::memory_order_relaxed);
            return chatwire::response::success(chatwire::json::object(*found));
        }

        [[nodiscard]] static auto tab() -> chatwire::response
        {
            auto rows{ chatwire::sdk::tab_list() };
            g_tab_queries.fetch_add(1, std::memory_order_relaxed);
            const std::size_t count{ rows.size() };
            return chatwire::response::success(chatwire::json::object(
                tab_list_result{ .count = count, .players = std::move(rows) }));
        }
    };
}

namespace chatwire::features::scoreboard
{
    /* @brief This feature's counters, for `system.stats`. */
    [[nodiscard]] inline auto stats() noexcept -> chatwire::features::scoreboard_stats
    {
        return chatwire::features::scoreboard_stats{
            .scoreboard_queries =
                chatwire::features::g_scoreboard_queries.load(std::memory_order_relaxed),
            .team_queries = chatwire::features::g_team_queries.load(std::memory_order_relaxed),
            .tab_queries  = chatwire::features::g_tab_queries.load(std::memory_order_relaxed) };
    }

    /* @brief This feature's singleton, for the root module to register. */
    [[nodiscard]] inline auto instance() noexcept -> chatwire::feature*
    {
        static chatwire::features::scoreboard_feature feature{};
        return &feature;
    }
}
