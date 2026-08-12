export module chatwire.features.rewrite;
import std;
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
// EVERY SURFACE A NAME APPEARS ON, WHICH IS THREE HOOKS AND NOT ONE
// ===========================================================================
// A name is drawn in four places and they do not share a code path:
//
//   tab list      GuiPlayerTabOverlay -> ScorePlayerTeam.formatPlayerName
//   sidebar       GuiIngame           -> ScorePlayerTeam.formatPlayerName
//   above a head  Render.renderLivingLabel, with the text ALREADY BUILT
//   chat          GuiNewChat.printChatMessage, an IChatComponent
//
// The first two share a hook; the other two need their own.  Rewriting only the
// team formatter -- which is what this feature did first -- changed the tab and
// the sidebar and left the name over the player's head untouched, which looks
// exactly like a rule that did not match.
//
// The rules are the same set for all three.  What differs is WHAT each hook
// hands over: formatPlayerName gets the raw name, renderLivingLabel gets the
// finished label with its colours in, and chat gets a whole line.  A rule
// matching `alpha` therefore sees `alpha` in one and `§calpha§r` in another,
// which is why `match` is a substring test rather than an equality one.
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
        /*
            WHOSE name this rewrites.  Empty means every name, which is the old
            behaviour and almost never what someone wants: "half the players"
            needs a rule per player, or a rule whose match names them.
        */
        std::string   match{};
        /*
            WHAT to draw instead, with placeholders resolved at draw time:

                {name}      the raw name Minecraft was about to decorate
                {health}    out of 20, one decimal
                {food}      out of 20
                {ping}      milliseconds
                {x} {y} {z} position, one decimal

            So `{name} {health}` turns `alpha` into `alpha 18.5`.  A template
            with no placeholder is a plain replacement, which is what the old
            find/replace did.
        */
        std::string   pattern{};
        std::string   find{};
        std::string   replace{};
        /*
            DO NOT DRAW IT AT ALL.  A drop rule carries a `match` and nothing
            else: there is no replacement to build, because the thing it matched
            never reaches the screen.  It is the same rule set and the same
            substring test as every other rule, which is what makes "hide every
            line from this bot" the same shape of request as "call this player
            something else" rather than a second feature with its own vocabulary.

            What each surface can actually do about it differs, and sdk.ixx says
            so at each detour: a chat line and a floating nametag are simply not
            drawn, while a tab or sidebar row -- drawn from a method's RETURN
            value -- collapses to its team's prefix and suffix instead.
        */
        bool          drop{ false };
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

        `dropped` is the same number for the rules that hide rather than change,
        and it is reported separately because a drop leaves NOTHING behind to
        look at.  A rewritten name can be checked by reading it; a hidden line
        can only be counted, so if this does not move, the rule is not matching.
    */
    struct rewrite_list_result
    {
        std::size_t               count{ 0 };
        std::uint64_t             applied{ 0 };
        std::uint64_t             dropped{ 0 };
        std::vector<rewrite_rule> rules{};
    };

    /* @brief This feature's contribution to `system.stats`. */
    struct rewrite_stats
    {
        std::uint64_t rewrites_applied{ 0 };
        std::uint64_t rewrites_dropped{ 0 };
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

            NOT `inline` -- see the note on chatwire::registry::detail::storage()
            in chatwire.feature.  One rule set per consumer would mean the rules
            a client adds and the rules the detour reads are different objects.
        */
        [[nodiscard]] auto rules() noexcept -> std::atomic<std::shared_ptr<const rule_set>>&
        {
            static auto* const live{
                new std::atomic<std::shared_ptr<const rule_set>>{
                    std::make_shared<const rule_set>() } };
            return *live;
        }

        inline std::atomic<std::uint64_t> g_next_id{ 1 };
        inline std::atomic<std::uint64_t> g_applied{ 0 };
        inline std::atomic<std::uint64_t> g_dropped{ 0 };

        /*
            @brief The player snapshot the detour reads.  Swapped whole.
            @details
            `{health}` has to come from somewhere and it must not be the detour:
            that runs on the render thread several times a frame, and reading a
            player's health means walking the entity list and calling Java.  A
            refresher takes the snapshot off-thread; the detour loads a pointer.
        */
        using snapshot_set = std::vector<chatwire::sdk::player_snapshot>;

        inline auto snapshots() noexcept
            -> std::atomic<std::shared_ptr<const snapshot_set>>&
        {
            static auto* const live{ new std::atomic<std::shared_ptr<const snapshot_set>>{
                std::make_shared<const snapshot_set>() } };
            return *live;
        }

        inline std::atomic<bool>   g_refresh_stop{ false };
        inline std::atomic<bool>   g_needs_live{ false };
        inline std::thread         g_refresher{};

        /* @brief True when any rule uses a placeholder that changes per tick. */
        [[nodiscard]] inline auto uses_live_data(const std::string_view pattern) noexcept -> bool
        {
            for (const std::string_view token : { "{health}", "{food}", "{ping}",
                                                  "{x}", "{y}", "{z}" })
            {
                if (pattern.find(token) != std::string_view::npos) { return true; }
            }
            return false;
        }

        /*
            @brief Renders one template for one name.  Detour, so: no allocation
                   beyond the result, no lock, no Java.
        */
        [[nodiscard]] inline auto render(const std::string_view pattern,
                                         const std::string_view name) -> std::string
        {
            const std::shared_ptr<const snapshot_set> live{
                snapshots().load(std::memory_order_acquire) };
            const chatwire::sdk::player_snapshot* found{ nullptr };
            if (live)
            {
                for (const auto& who : *live)
                {
                    if (who.name == name) { found = &who; break; }
                }
            }

            std::string out;
            out.reserve(pattern.size() + 16u);
            for (std::size_t i{ 0 }; i < pattern.size(); ++i)
            {
                if (pattern[i] != '{') { out += pattern[i]; continue; }
                const std::size_t close{ pattern.find('}', i) };
                if (close == std::string_view::npos) { out += pattern[i]; continue; }

                const std::string_view token{ pattern.substr(i + 1u, close - i - 1u) };
                if (token == "name") { out += name; }
                else if (!found)
                {
                    // A placeholder we cannot fill yet -- the snapshot has not
                    // caught up, or that name is not a player.  The token is
                    // dropped rather than printed, so a nametag never reads
                    // "alpha {health}" at somebody.
                }
                else if (token == "health") { out += std::format("{:.1f}", found->health); }
                else if (token == "food")   { out += std::format("{}", found->food); }
                else if (token == "ping")   { out += std::format("{}", found->ping); }
                else if (token == "x")      { out += std::format("{:.1f}", found->x); }
                else if (token == "y")      { out += std::format("{:.1f}", found->y); }
                else if (token == "z")      { out += std::format("{:.1f}", found->z); }
                i = close;
            }
            return out;
        }

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
        inline auto rewrite_name(const char* const original, std::string& replacement)
            -> chatwire::sdk::rewrite_outcome
        {
            using outcome = chatwire::sdk::rewrite_outcome;
            try
            {
                const std::shared_ptr<const rule_set> live{
                    rules().load(std::memory_order_acquire) };
                if (!live || live->empty() || !original) { return outcome::unchanged; }

                const std::string_view source{ original };
                bool changed{ false };
                for (const rewrite_rule& rule : *live)
                {
                    // WHOSE name is this?  An empty match is every name; a
                    // non-empty one has to appear in it, which is what lets a
                    // rule apply to one player and not the next.
                    if (!rule.match.empty()
                        && source.find(rule.match) == std::string_view::npos)
                    {
                        continue;
                    }

                    if (rule.drop)
                    {
                        // A DROP ENDS THE LOOP.  Nothing a later rule could do
                        // to text that is not going to be drawn is worth the
                        // work, and answering "dropped" while also handing back
                        // a replacement would leave two callers to decide which
                        // of the two they believed.  First drop wins, and it
                        // wins over any rewrite -- registered before it or
                        // after.
                        g_dropped.fetch_add(1, std::memory_order_relaxed);
                        return outcome::dropped;
                    }

                    if (!rule.pattern.empty())
                    {
                        // A TEMPLATE replaces the whole name rather than editing
                        // it, because `{name} {health}` is not a substitution --
                        // it is a new string built from the old one.
                        replacement = render(rule.pattern, source);
                        changed = true;
                        continue;
                    }
                    if (!changed) { replacement.assign(source); }
                    changed = substitute(replacement, rule.find, rule.replace) || changed;
                }
                if (changed) { g_applied.fetch_add(1, std::memory_order_relaxed); }
                return changed ? outcome::replaced : outcome::unchanged;
            }
            catch (...) { return outcome::unchanged; }
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
        /*
            @brief Keeps the snapshot the templates read from going stale.
            @details
            Runs only while a rule actually uses a live placeholder -- a rule
            that just renames somebody needs no snapshot, and taking one twice a
            second for nothing would be a java_thread_scope the whole VM waits on
            for no reason.

            Off the render thread, deliberately: this is the work the detour is
            not allowed to do.
        */
        static auto refresh_loop() noexcept -> void
        {
            while (!detail::g_refresh_stop.load(std::memory_order_acquire))
            {
                for (int tick{ 0 }; tick < 5; ++tick)
                {
                    if (detail::g_refresh_stop.load(std::memory_order_acquire)) { return; }
                    std::this_thread::sleep_for(std::chrono::milliseconds{ 100 });
                }
                if (!detail::g_needs_live.load(std::memory_order_acquire)) { continue; }
                try
                {
                    detail::snapshots().store(
                        std::make_shared<const detail::snapshot_set>(
                            chatwire::sdk::player_snapshots()),
                        std::memory_order_release);
                }
                catch (...) { }
            }
        }

        [[nodiscard]] auto start() noexcept -> bool override
        {
            if (!chatwire::sdk::install_name_rewriter(&detail::rewrite_name))
            {
                chatwire::log::warn("rewrite: could not hook the name formatter; "
                                    "display names cannot be changed");
                return false;
            }
            // The FLOATING nametag is a separate draw path -- Render
            // .renderLivingLabel, with the text already built -- so it needs its
            // own hook.  Not fatal if it fails: the tab and the sidebar still
            // rewrite, and saying which half is missing beats failing both.
            if (!chatwire::sdk::install_label_rewriter())
            {
                chatwire::log::warn("rewrite: could not hook the nametag renderer; "
                                    "the tab list and sidebar still rewrite, the name "
                                    "above a player's head does not");
            }
            detail::g_refresh_stop.store(false, std::memory_order_release);
            try { detail::g_refresher = std::thread{ &rewrite_feature::refresh_loop }; }
            catch (...) { }
            return true;
        }

        auto stop() noexcept -> void override
        {
            // The rules go, so that a chatwire started again in this process
            // does not inherit somebody's substitutions.  The HOOK comes down
            // centrally, in sdk::remove_hooks, after every thread that could be
            // inside it has been joined.
            (void)detail::update([](detail::rule_set& set) { set.clear(); return 0u; });
            // JOINED, not signalled and forgotten: it runs code in this DLL and
            // the DLL is about to be unloaded.
            detail::g_refresh_stop.store(true, std::memory_order_release);
            try { if (detail::g_refresher.joinable()) { detail::g_refresher.join(); } }
            catch (...) { }
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
            const auto match{ chatwire::json::get_string(cmd.body, "match") };
            const auto pattern{ chatwire::json::get_string(cmd.body, "template") };
            const auto find{ chatwire::json::get_string(cmd.body, "find") };
            const auto replace{ chatwire::json::get_string(cmd.body, "replace") };
            const bool drop{ chatwire::json::get_bool(cmd.body, "drop").value_or(false) };

            // Three shapes now.  `drop` is checked FIRST and refuses to share a
            // rule with the other two: a rule that hides something has no use
            // for what to draw instead, and accepting both would mean silently
            // ignoring one of them at the moment somebody most wants to know
            // which of the two they got.
            if (drop)
            {
                if (pattern || find || replace)
                {
                    return chatwire::response::failure(
                        "'drop' takes a 'match' and nothing else -- a rule that hides "
                        "something has nothing to draw instead");
                }
                // A DROP WITHOUT A MATCH IS REFUSED, and this is the one place
                // this feature refuses an empty match.  Everywhere else it means
                // "every name", which is a loud and reversible mistake -- you
                // can see what it did.  A drop with no match hides every chat
                // line and every nametag in the game AT ONCE, and what it looks
                // like from inside is a client that has silently stopped
                // working.  Nobody means that, so it cannot be typed by
                // accident.
                if (!match || match->empty())
                {
                    return chatwire::response::failure(
                        "'drop' needs a non-empty 'match' -- a rule that hides "
                        "everything is never what anyone means");
                }
            }
            // Two shapes for the rest, and the reply says which one it took.
            // `template` is the one that can build a name out of live values;
            // `find`/`replace` is a plain substitution and stays because it is
            // the shortest way to say "call them something else".
            else if (!pattern && !find)
            {
                return chatwire::response::failure(
                    "give either 'template' (with {name}, {health}, {food}, {ping}, "
                    "{x}, {y}, {z}), or 'find' and 'replace', or 'drop' with a 'match'");
            }
            else if (!pattern && (find->empty() || !replace))
            {
                return chatwire::response::failure("'find' needs a matching 'replace'");
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
                set.push_back(rewrite_rule{ .id = id,
                                            .match = match ? *match : std::string{},
                                            .pattern = pattern ? *pattern : std::string{},
                                            .find = find ? *find : std::string{},
                                            .replace = replace ? *replace : std::string{},
                                            .drop = drop,
                                            .owner = cmd.client });
                return 1u;
            });
            // Does anything now need a live snapshot?  Recomputed over the whole
            // set rather than latched on, so removing the last live rule stops
            // the refresher paying for one.
            {
                const auto live{ detail::rules().load(std::memory_order_acquire) };
                bool needed{ false };
                if (live)
                {
                    for (const auto& r : *live)
                    {
                        if (detail::uses_live_data(r.pattern)) { needed = true; break; }
                    }
                }
                detail::g_needs_live.store(needed, std::memory_order_release);
            }
            // `*find` is only safe to read once `drop` and `template` have both
            // been ruled out -- a drop rule carries neither, and reaching into
            // an empty optional to log it would be a crash in the one line
            // whose job is to say what just happened.
            chatwire::log::info("rewrite rule {}: match '{}' -> '{}'", id,
                                match ? *match : "(everyone)",
                                drop      ? std::string{ "(not drawn)" }
                                : pattern ? *pattern
                                          : std::format("{} -> {}", *find, *replace));
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
                .dropped = detail::g_dropped.load(std::memory_order_relaxed),
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
            .rewrites_dropped =
                chatwire::features::detail::g_dropped.load(std::memory_order_relaxed),
            .rewrite_rules = live ? live->size() : 0u };
    }

    /* @brief This feature's singleton, for the root module to register. */
    [[nodiscard]] inline auto instance() noexcept -> chatwire::feature*
    {
        static chatwire::features::rewrite_feature feature{};
        return &feature;
    }
}
