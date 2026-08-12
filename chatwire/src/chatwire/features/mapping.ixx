module;

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>
// <meta> in THIS unit's fragment, even though chatwire.reflect also has it.
// A module does not re-export what its global module fragment included, so
// `import chatwire.reflect;` brings the wrapper and not std::meta itself.
#include <meta>

export module chatwire.features.mapping;
import chatwire.reflect;
import chatwire.feature;
import chatwire.json;
import chatwire.log;
import chatwire.mapping;
import chatwire.sdk;

// chatwire.features.mapping — chatwire checking its own name table against the
// client it actually landed in.
//
// ===========================================================================
// THE FAILURE THIS EXISTS TO MAKE LOUD
// ===========================================================================
// mapping.hpp opens with the reason: a wrong OBF name fails SILENTLY.  The
// class resolves to something real, the member lookup finds nothing, and the
// feature that needed it just never works.  That is not hypothetical -- `avq`
// shipped here for a release as GuiNewChat, which is really MapItemRenderer, so
// every user on a vanilla client got a bridge that could send chat and never
// reported any.  Nothing failed.  Nothing logged.  It looked like a bridge that
// worked.
//
// Three things had to be true for that to survive:
//
//   1. nothing could ENUMERATE the table, so nothing could check it;
//   2. a missing name and a name that is simply unused look identical;
//   3. the mapping most users run is the one a developer never runs.
//
// The first is now false.  mapping::table is a struct, so
// std::meta::nonstatic_data_members_of walks it, and `mapping.verify` asks the
// attached JVM about every entry in one request:
//
//     {"cmd":"mapping.verify"}
//     -> {"ok":true,"result":{"mapping":"OBF (vanilla obfuscated)",
//                             "checked":23,"missing":0,"entries":[...]}}
//
// The third is why this is a COMMAND rather than an assertion at start-up: the
// answer differs per client, and the client that matters is somebody else's.
// A user can run one command and paste the result.
//
// ===========================================================================
// WHY THIS FEATURE KEEPS A SHORT NAME
// ===========================================================================
// Every command in chatwire is the fully-qualified Java member it reaches, so
// that it can be checked against Minecraft's source.  `mapping.*` reaches no
// single member -- it asks about ALL of them -- so there is nothing to spell
// out, exactly as with `system.*`.  Those two are the only ones.
//
// ===========================================================================
// COST
// ===========================================================================
// `verify` is metaspace reads and nothing else: no oop is touched, nothing is
// called, so it needs no thread state and no java_thread_scope, and it cannot
// disturb the game.  It is still not free -- find_class walks the
// ClassLoaderDataGraph on a miss, which on a heavily modded client is
// measurable -- so it is a request a user makes, never something polled.

// Brings <meta> with it: this feature GENERATES its work from the table's shape.
// See the note at the bottom of common.hpp for why that include is confined.





export namespace chatwire::features
{
    namespace map = chatwire::mapping;

    /*
        @brief One row of `mapping.verify`.
        @details
        `spelling` is what the name is called under the DETECTED mapping, which
        is the string that was actually looked up -- reporting the MCP spelling
        of a lookup that used the OBF one would hide the interesting half.

        `kind` is what the JVM turned out to have: "class", "field", "method",
        or "absent".  A field where the table means a method is not an error
        here and is not reported as one: the lookup this checks is by name, and
        a name that exists is a name that resolves.
    */
    struct mapping_entry_report
    {
        std::string_view group{};
        std::string_view member{};
        std::string      spelling{};
        std::string_view kind{};
        bool             found{ false };
    };

    /*
        @brief The answer to `mapping.verify`.
        @details
        `missing` and `unchecked` are different failures and only the first is
        chatwire's fault.

        A class Minecraft has NOT LOADED YET cannot be found by any name, right
        or wrong -- `find_class` walks loaded classes, which is all a running JVM
        can be asked about.  FoodStats is the honest example: nothing loads it
        until a player exists, so on the title screen its three names read as
        absent and look exactly like the `avq` mistake.  They are not.  Those
        entries are reported `unchecked`, and their group's class is reported
        `not loaded`, so a reader can tell "this name is wrong" from "ask me
        again once you are in a world".

        Which is also the advice: verify in a world.  On the title screen a third
        of the table is simply not loaded.
    */
    struct mapping_verify_result
    {
        std::string_view                  mapping{};
        std::size_t                       checked{ 0 };
        std::size_t                       missing{ 0 };
        std::size_t                       unchecked{ 0 };
        std::vector<mapping_entry_report> entries{};
    };

    /*
        @brief The answer to `mapping.detected` — the decision AND its evidence.
        @details
        The four booleans are the probes mapping::decide runs on, reported
        as-is.  On an `unknown` client they are the whole diagnosis: no
        Minecraft class at all means it is not 1.8.9, while the class present
        and neither field found means it is a build whose members have been
        renamed by something that is not MCP.
    */
    struct mapping_detected_result
    {
        std::string_view mapping{};
        bool             minecraft_class{ false };
        bool             mcp_field{ false };
        bool             srg_field{ false };
        bool             obf_class{ false };
    };

    /* @brief The answer to `mapping.resolve`. */
    struct mapping_resolve_result
    {
        std::string_view group{};
        std::string_view member{};
        std::string_view mcp{};
        std::string_view srg{};
        std::string_view obf{};
        std::string      spelling{};
        std::string_view kind{};
        bool             found{ false };
    };

    namespace detail
    {
        /*
            @brief Probes one group's entries and appends a row for each.
            @details
            THE CONVENTION, and the only thing this walk assumes about the
            table: the member called `clazz` is the class, and every other
            member is a field or a method on it.  mapping.hpp says the same
            thing where the groups are declared, because a convention stated in
            one place is a convention nobody can see.

            `if constexpr` on the identifier rather than a runtime `if`: an
            expansion statement is not a loop, so there is no `continue` to
            reach for, and the two branches want different probes anyway.
        */
        template<typename group_type>
        auto verify_group(const std::string_view group_name, const group_type& group,
                          std::vector<mapping_entry_report>& out) -> void
        {
            const std::string class_name{ map::resolve(group.clazz) };
            // Asked ONCE, before the members: every member's verdict depends on
            // it, and find_class walks the ClassLoaderDataGraph on a miss -- so
            // a group whose class is not loaded would otherwise pay for that
            // walk once per member to reach the same answer.
            const bool loaded{ chatwire::sdk::class_exists(class_name) };

            template for (constexpr auto member : chatwire::reflect::members_of<group_type>())
            {
                constexpr std::string_view id{ chatwire::reflect::identifier<member>() };

                mapping_entry_report row{ .group = group_name, .member = id,
                                          .spelling = map::resolve(group.[:member:]) };

                if constexpr (id == "clazz")
                {
                    row.found = loaded;
                    row.kind  = loaded ? "class" : "not loaded";
                }
                else if (!loaded)
                {
                    row.kind = "unchecked";
                }
                else
                {
                    const auto kind{ chatwire::sdk::find_member(class_name, row.spelling) };
                    row.found = kind != chatwire::sdk::member_kind::absent;
                    row.kind  = chatwire::sdk::member_kind_name(kind);
                }

                out.push_back(std::move(row));
            }
        }

        /* @brief Every entry in the table, checked against the attached JVM. */
        [[nodiscard]] inline auto verify_table() -> std::vector<mapping_entry_report>
        {
            std::vector<mapping_entry_report> out;
            template for (constexpr auto group : chatwire::reflect::members_of<map::table>())
            {
                constexpr std::string_view group_name{ chatwire::reflect::identifier<group>() };
                verify_group(group_name, map::all.[:group:], out);
            }
            return out;
        }

        /*
            @brief The resolved class name of one group, by the group's own name.
            @details
            Every group has a `clazz` -- that is the convention verify_group
            relies on -- so this is total over the table, and "" for a group
            name that is not in it.
        */
        [[nodiscard]] inline auto class_of(const std::string_view group_name) -> std::string
        {
            std::string out;
            template for (constexpr auto group : chatwire::reflect::members_of<map::table>())
            {
                if (group_name == chatwire::reflect::identifier<group>())
                {
                    out = map::resolve(map::all.[:group:].clazz);
                }
            }
            return out;
        }

        /*
            @brief Finds the table entry `query` names, in any mapping.
            @details
            Matched against all four spellings a caller might have: the three
            names themselves, and the C++ identifier the table calls it by --
            so `thePlayer`, `field_71439_g`, `h` and `the_player` all reach the
            same row, as does `minecraft.the_player` for a name whose short form
            is ambiguous.

            `h` IS ambiguous, and deliberately answered anyway: it is the OBF
            spelling of several unrelated members, and the first match in table
            order is the one this returns.  A caller who cares which spells the
            group.
        */
        [[nodiscard]] inline auto resolve_query(const std::string_view query)
            -> std::optional<mapping_resolve_result>
        {
            std::optional<mapping_resolve_result> found;

            template for (constexpr auto group : chatwire::reflect::members_of<map::table>())
            {
                constexpr std::string_view group_name{ chatwire::reflect::identifier<group>() };
                const auto& g{ map::all.[:group:] };

                template for (constexpr auto member :
                              chatwire::reflect::members_of<std::remove_cvref_t<decltype(g)>>())
                {
                    constexpr std::string_view id{ chatwire::reflect::identifier<member>() };
                    const map::name& n{ g.[:member:] };

                    // No `continue` -- an expansion statement is not a loop --
                    // so the guard is "have not found one yet" rather than an
                    // early exit.  The table is two dozen entries; walking the
                    // rest of it costs nothing worth a control-flow trick.
                    if (!found
                        && (query == n.mcp || query == n.obf || query == n.srg
                            || query == id
                            || (query.size() > group_name.size()
                                && query.starts_with(group_name)
                                && query[group_name.size()] == '.'
                                && query.substr(group_name.size() + 1) == id)))
                    {
                        found = mapping_resolve_result{
                            .group = group_name, .member = id,
                            .mcp = n.mcp, .srg = n.srg, .obf = n.obf,
                            .spelling = map::resolve(n) };
                    }
                }
            }
            return found;
        }
    }

    class mapping_feature final : public chatwire::feature
    {
    public:
        [[nodiscard]] auto name() const noexcept -> std::string_view override
        {
            return "mapping";
        }

        /* Nothing to install: this feature reads metaspace, it does not hook. */
        [[nodiscard]] auto start() noexcept -> bool override { return true; }
        auto stop() noexcept -> void override { }

        [[nodiscard]] auto handle(const chatwire::command& cmd) noexcept
            -> chatwire::response override
        {
            try
            {
                if (cmd.verb == "detected") { return this->detected(); }
                if (cmd.verb == "verify")   { return this->verify(); }
                if (cmd.verb == "resolve")  { return this->resolve(cmd); }

                return chatwire::response::failure(
                    "unknown verb; try detected, verify or resolve");
            }
            catch (...)
            {
                return chatwire::response::failure("internal error");
            }
        }

    private:
        [[nodiscard]] static auto detected() -> chatwire::response
        {
            // Re-probed rather than reported from `mapping::current`: the point
            // of this verb is what the JVM says NOW, and a cached answer from
            // injection time would be the one thing it must not be.  decide()
            // is not called, so nothing here can change what the rest of
            // chatwire is resolving against mid-session.
            const auto probe{ chatwire::sdk::probe_mapping() };
            return chatwire::response::success(
                chatwire::json::object(mapping_detected_result{
                    .mapping         = map::mode_name(map::current),
                    .minecraft_class = probe.mcp_class_present,
                    .mcp_field       = probe.mcp_field_present,
                    .srg_field       = probe.srg_field_present,
                    .obf_class       = probe.obf_class_present }));
        }

        [[nodiscard]] static auto verify() -> chatwire::response
        {
            if (map::current == map::mode::unknown)
            {
                return chatwire::response::failure(
                    "no mapping was detected, so there is nothing to verify against");
            }

            auto entries{ detail::verify_table() };
            const auto counted{ [&entries](const std::string_view kind) noexcept
            {
                return static_cast<std::size_t>(std::ranges::count_if(
                    entries, [kind](const auto& e) { return e.kind == kind; }));
            } };
            const std::size_t unchecked{ counted("unchecked") + counted("not loaded") };
            const std::size_t missing{ counted("absent") };

            // Logged as well as answered, and at warn: a client asking this
            // question is usually a user diagnosing something, and the answer
            // belongs in the log they will be asked for next.
            if (missing != 0u)
            {
                chatwire::log::warn("mapping.verify: {} of {} names are absent under {}",
                                    missing, entries.size(), map::mode_name(map::current));
            }

            return chatwire::response::success(
                chatwire::json::object(mapping_verify_result{
                    .mapping   = map::mode_name(map::current),
                    .checked   = entries.size(),
                    .missing   = missing,
                    .unchecked = unchecked,
                    .entries   = std::move(entries) }));
        }

        [[nodiscard]] static auto resolve(const chatwire::command& cmd) -> chatwire::response
        {
            const auto query{ chatwire::json::get_string(cmd.body, "name") };
            if (!query || query->empty())
            {
                return chatwire::response::failure("missing or non-string 'name'");
            }

            auto row{ detail::resolve_query(*query) };
            if (!row)
            {
                return chatwire::response::failure(
                    "no entry with that name; chatwire's table only carries the "
                    "names it uses");
            }

            if (row->member == "clazz")
            {
                row->found = chatwire::sdk::class_exists(row->spelling);
                row->kind  = row->found ? "class" : "absent";
            }
            else
            {
                const auto kind{ chatwire::sdk::find_member(
                    detail::class_of(row->group), row->spelling) };
                row->found = kind != chatwire::sdk::member_kind::absent;
                row->kind  = chatwire::sdk::member_kind_name(kind);
            }

            return chatwire::response::success(chatwire::json::object(*row));
        }
    };
}

export namespace chatwire::features::mapping
{
    /* @brief This feature's singleton, for the root module to register. */
    [[nodiscard]] inline auto instance() noexcept -> chatwire::feature*
    {
        static chatwire::features::mapping_feature feature{};
        return &feature;
    }
}
