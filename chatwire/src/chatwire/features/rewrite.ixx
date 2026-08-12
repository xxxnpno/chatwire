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
// <meta> HERE TOO.  A reflection template is instantiated in the unit that
// CALLS it, not in the one that defines it, so every module that reaches
// json::object or config's walkers needs the header in its own fragment --
// importing chatwire.reflect is not enough, and GCC's error points into
// libstdc++ rather than at the missing include.
#include <meta>

export module chatwire.features.rewrite;
import chatwire.feature;
import chatwire.json;
import chatwire.log;
import chatwire.sdk;

// chatwire.features.rewrite — changing what the player is shown.
//
// ===========================================================================
// WHAT IT IS
// ===========================================================================
// Every other feature READS the game or sends something into it.  This one
// edits the game's own output on its way to the screen: a client registers a
// rule over the socket and, from that moment, every name Minecraft is about to
// draw goes through it.
//
//     {"cmd":"rewrite.add","find":"rouge","replace":"bleu"}
//
// and a player called `rouge_42` is drawn as `bleu_42` — above their head, and
// in the tab list — until the rule is removed.  Nothing is sent to the server
// and no other player sees anything change; this is the client's own display.
//
// ===========================================================================
// WHERE IT HOOKS, AND WHY THAT ONE PLACE
// ===========================================================================
// `ScorePlayerTeam.formatPlayerName(Team, String)`.  It is static, and it is
// the single funnel every decorated name in 1.8.9 passes through:
// `RendererLivingEntity.renderName` builds a nametag with it, and
// `GuiPlayerTabOverlay.getPlayerName` builds a tab row with it.  Hooking there
// rather than at the two call sites is what makes a rule consistent between
// them by construction rather than by remembering.
//
// THE ARGUMENT IS REWRITTEN, NOT THE RETURN.  The raw name goes in changed and
// Minecraft does the decorating, so a rewritten name still gets its team's
// colour, prefix and suffix exactly as the game would have applied them.
// Forcing the return instead would mean reproducing that formatting here and
// getting it wrong for the first server that uses a corner of it.
//
// WHAT IT DOES NOT REACH, said plainly because a client will notice.  A tab row
// for which the server has PUSHED a display name does not go through
// formatPlayerName at all — Minecraft draws the component it was sent.  So a
// rule always changes nametags, and changes tab rows on servers that leave the
// name alone.  `getPlayerInfoMap` reports both `display_name` and the complete
// `line`, which is how a caller can see which case it is looking at.
//
// ===========================================================================
// THE RULES LIVE WHERE A DETOUR CAN READ THEM
// ===========================================================================
// The hook runs on the RENDER THREAD, several times a frame — once per visible
// nametag and once per tab row.  It must not take a lock a socket thread might
// be holding, and it must not allocate when nothing matches.
//
// So the rule set is immutable and swapped WHOLE: an edit builds a new vector
// and stores a new shared_ptr, and the detour loads the pointer atomically and
// reads it.  No lock, no torn read, and a rule that is being removed stays
// valid for as long as the detour that is using it.
//
// ===========================================================================
// A RULE BELONGS TO THE CONNECTION THAT ADDED IT
// ===========================================================================
// and is withdrawn when that connection closes, exactly like a registered
// command.  That is deliberate rather than convenient: a rule nobody owns is a
// game quietly lying to its player with no way left to ask why.  A tool that
// wants a rewrite to outlive it should stay connected.




export namespace chatwire::features
{
    /* @brief One registered substitution. */
    struct rewrite_rule
    {
        std::uint64_t id{ 0 };
        std::string   find{};
        std::string   replace{};
        /* Which connection registered it.  See the note at the top. */
        std::uint64_t owner{ 0 };
    };

    /* @brief What `rewrite.add` answers with. */
    struct rewrite_added { std::uint64_t id{ 0 }; };
    /* @brief What `rewrite.remove` and `rewrite.clear` answer with. */
    struct rewrite_removed { std::size_t removed{ 0 }; };

    /*
        @brief What `rewrite.list` answers with.
        @details
        `applied` is how many names have actually been changed since chatwire
        started, which is the number that says whether a rule is doing anything.
        A rule that matches nothing looks identical to a rule that was never
        registered, and this is the difference.
    */
    struct rewrite_list_result
    {
        std::size_t               count{ 0 };
        std::uint64_t             applied{ 0 };
        std::vector<rewrite_rule> rules{};
    };

    /* @brief This feature's contribution to `system.stats`. */
    struct rewrite_stats
    {
        std::uint64_t rewrites_applied{ 0 };
        std::uint64_t rewrite_rules{ 0 };
    };

    namespace detail
    {
        using rule_set = std::vector<rewrite_rule>;

        /*
            @brief The live rule set.  Swapped whole, never edited in place.
            @details
            A function-local static rather than a namespace-scope one, for the
            same reason the feature registry is: this is read from a detour that
            can run before or after any other translation unit's static
            initialisation.
        */
        inline auto rules() noexcept -> std::atomic<std::shared_ptr<const rule_set>>&
        {
            static auto* const live{
                new std::atomic<std::shared_ptr<const rule_set>>{
                    std::make_shared<const rule_set>() } };
            return *live;
        }

        inline std::atomic<std::uint64_t> g_next_id{ 1 };
        inline std::atomic<std::uint64_t> g_applied{ 0 };

        /* @brief Replaces every occurrence of `find` in `text`. */
        inline auto substitute(std::string& text, const std::string_view find,
                               const std::string_view replace) -> bool
        {
            if (find.empty()) { return false; }
            bool changed{ false };
            for (std::size_t at{ text.find(find) }; at != std::string::npos;
                 at = text.find(find, at + replace.size()))
            {
                text.replace(at, find.size(), replace);
                changed = true;
                // `at + replace.size()` above, not `at + 1`: a rule whose
                // replacement contains its own pattern -- find "a", replace
                // "aa" -- would otherwise rewrite what it just wrote, forever,
                // on the render thread.
            }
            return changed;
        }

        /*
            @brief The detour's callback.  Render thread, several times a frame.
            @details
            Loads the rule set atomically and applies each rule in registration
            order, so a later rule sees what an earlier one produced -- which is
            what "and also" means when someone adds a second rule.

            Allocates NOTHING when no rule matches, which is the overwhelmingly
            common case: the first match is what creates the string.
        */
        inline auto rewrite_name(const char* const original, std::string& replacement) -> bool
        {
            try
            {
                const std::shared_ptr<const rule_set> live{
                    rules().load(std::memory_order_acquire) };
                if (!live || live->empty() || !original) { return false; }

                const std::string_view source{ original };
                bool changed{ false };
                for (const rewrite_rule& rule : *live)
                {
                    if (!changed && source.find(rule.find) == std::string_view::npos)
                    {
                        continue;                       // no match, nothing built yet
                    }
                    if (!changed) { replacement.assign(source); }
                    changed = substitute(replacement, rule.find, rule.replace) || changed;
                }
                if (changed) { g_applied.fetch_add(1, std::memory_order_relaxed); }
                return changed;
            }
            catch (...) { return false; }
        }

        /* @brief Swaps in a rule set built by `edit` from the current one. */
        template<typename edit_type>
        inline auto update(edit_type&& edit) -> std::size_t
        {
            const std::shared_ptr<const rule_set> before{
                rules().load(std::memory_order_acquire) };
            auto after{ std::make_shared<rule_set>(before ? *before : rule_set{}) };
            const std::size_t changed{ edit(*after) };
            rules().store(std::shared_ptr<const rule_set>{ std::move(after) },
                          std::memory_order_release);
            return changed;
        }
    }

    class rewrite_feature final : public chatwire::feature
    {
    public:
        [[nodiscard]] auto name() const noexcept -> std::string_view override
        {
            return "rewrite";
        }

        /*
            @brief Keeps its own short prefix, like `system` and `commands`.
            @details
            A rule is not one Java member: it applies to whatever the game is
            about to draw, and the member it hooks is an implementation detail
            that could reasonably grow to several.  Naming the command after
            `formatPlayerName` would promise a precision this does not have.
        */
        [[nodiscard]] auto start() noexcept -> bool override
        {
            if (!chatwire::sdk::install_name_rewriter(&detail::rewrite_name))
            {
                chatwire::log::warn("rewrite: could not hook the name formatter; "
                                    "display names cannot be changed");
                return false;
            }
            return true;
        }

        auto stop() noexcept -> void override
        {
            // The rules go, so that a chatwire started again in this process
            // does not inherit somebody's substitutions.  The HOOK comes down
            // centrally, in sdk::remove_hooks, after every thread that could be
            // inside it has been joined.
            (void)detail::update([](detail::rule_set& set) { set.clear(); return 0u; });
        }

        [[nodiscard]] auto handle(const chatwire::command& cmd) noexcept
            -> chatwire::response override
        {
            try
            {
                if (cmd.verb == "add")    { return this->add(cmd); }
                if (cmd.verb == "remove") { return this->remove(cmd); }
                if (cmd.verb == "clear")  { return this->clear(cmd); }
                if (cmd.verb == "list")   { return this->list(); }

                return chatwire::response::failure("unknown verb; try add, remove, list or clear");
            }
            catch (...)
            {
                return chatwire::response::failure("internal error");
            }
        }

    private:
        [[nodiscard]] static auto add(const chatwire::command& cmd) -> chatwire::response
        {
            const auto find{ chatwire::json::get_string(cmd.body, "find") };
            const auto replace{ chatwire::json::get_string(cmd.body, "replace") };
            if (!find || find->empty())
            {
                return chatwire::response::failure("missing or empty 'find'");
            }
            if (!replace)
            {
                return chatwire::response::failure("missing or non-string 'replace'");
            }
            // A rule per frame per nametag is cheap; ten thousand of them are
            // not.  The bound is here rather than in the detour because the
            // detour must not have a reason to stop early.
            if (detail::rules().load(std::memory_order_acquire)->size() >= 64u)
            {
                return chatwire::response::failure("too many rules; remove some first");
            }

            const std::uint64_t id{ detail::g_next_id.fetch_add(1, std::memory_order_relaxed) };
            (void)detail::update([&](detail::rule_set& set)
            {
                set.push_back(rewrite_rule{ .id = id, .find = *find,
                                            .replace = *replace, .owner = cmd.client });
                return 1u;
            });
            chatwire::log::info("rewrite rule {}: '{}' -> '{}'", id, *find, *replace);
            return chatwire::response::success(
                chatwire::json::object(rewrite_added{ .id = id }));
        }

        [[nodiscard]] static auto remove(const chatwire::command& cmd) -> chatwire::response
        {
            const auto text{ chatwire::json::get_string(cmd.body, "id") };
            if (!text) { return chatwire::response::failure("missing 'id'"); }

            std::uint64_t wanted{ 0 };
            const auto* const first{ text->data() };
            const auto* const last{ text->data() + text->size() };
            if (std::from_chars(first, last, wanted).ptr != last)
            {
                return chatwire::response::failure("'id' is not a number");
            }

            const std::size_t gone{ detail::update([&](detail::rule_set& set)
            {
                const auto before{ set.size() };
                std::erase_if(set, [&](const rewrite_rule& r) { return r.id == wanted; });
                return before - set.size();
            }) };
            return chatwire::response::success(
                chatwire::json::object(rewrite_removed{ .removed = gone }));
        }

        [[nodiscard]] static auto clear(const chatwire::command& cmd) -> chatwire::response
        {
            // Only this connection's own rules, so one tool cannot silently
            // undo another's.  `forget_client` is the same erase, run when a
            // connection goes away.
            const std::size_t gone{ detail::update([&](detail::rule_set& set)
            {
                const auto before{ set.size() };
                std::erase_if(set, [&](const rewrite_rule& r) { return r.owner == cmd.client; });
                return before - set.size();
            }) };
            return chatwire::response::success(
                chatwire::json::object(rewrite_removed{ .removed = gone }));
        }

        [[nodiscard]] static auto list() -> chatwire::response
        {
            const std::shared_ptr<const detail::rule_set> live{
                detail::rules().load(std::memory_order_acquire) };
            detail::rule_set copy{ live ? *live : detail::rule_set{} };
            const std::size_t count{ copy.size() };
            return chatwire::response::success(chatwire::json::object(rewrite_list_result{
                .count   = count,
                .applied = detail::g_applied.load(std::memory_order_relaxed),
                .rules   = std::move(copy) }));
        }
    };
}

export namespace chatwire::features::rewrite
{
    /*
        @brief Drops every rule a departing connection registered.
        @details
        Called by the host from the presence handler, exactly as the commands
        feature's is.  See the note at the top of this file for why a rule does
        not outlive its owner.
    */
    inline auto forget_client(const std::uint64_t client) noexcept -> void
    {
        try
        {
            const std::size_t gone{ chatwire::features::detail::update(
                [&](chatwire::features::detail::rule_set& set)
                {
                    const auto before{ set.size() };
                    std::erase_if(set, [&](const chatwire::features::rewrite_rule& r)
                                  { return r.owner == client; });
                    return before - set.size();
                }) };
            if (gone != 0u)
            {
                chatwire::log::info("withdrew {} rewrite rule(s) with client {}", gone, client);
            }
        }
        catch (...) { }
    }

    /* @brief This feature's counters, for `system.stats`. */
    [[nodiscard]] inline auto stats() noexcept -> chatwire::features::rewrite_stats
    {
        const auto live{ chatwire::features::detail::rules().load(std::memory_order_acquire) };
        return chatwire::features::rewrite_stats{
            .rewrites_applied =
                chatwire::features::detail::g_applied.load(std::memory_order_relaxed),
            .rewrite_rules = live ? live->size() : 0u };
    }

    /* @brief This feature's singleton, for the root module to register. */
    [[nodiscard]] inline auto instance() noexcept -> chatwire::feature*
    {
        static chatwire::features::rewrite_feature feature{};
        return &feature;
    }
}
