export module chatwire.feature;
import std;
import chatwire.log;

// chatwire.core.feature — the extension point.
//
// ===========================================================================
// THE POINT
// ===========================================================================
// Chat is the first feature, not the only one.  Adding "inventory" or "world"
// or "players" later must NOT mean editing the server, the protocol dispatcher,
// or the injection entry point — every one of those edits is a chance to break
// chat, and a codebase where adding a feature means touching five files is a
// codebase that stops getting features.
//
// So a feature is a self-contained object that declares:
//
//     name()      what it is called in the log  ("chat")
//     claims()    which command prefixes it answers to
//     start()     install hooks, resolve mappings
//     stop()      take them down
//     handle()    respond to one client command
//
// and registers itself with one line at namespace scope.  The server discovers
// it, routes `net.minecraft.client.entity.EntityPlayerSP.sendChatMessage` to it,
// and nothing else in the project knows it exists.
//
// A second feature is literally: new module, implement the interface, register.
//
// ===========================================================================
// LIFECYCLE, AND WHY IT IS ORDERED THIS WAY
// ===========================================================================
// Registration happens at static-init time, so it must not touch a JVM — there
// may not be one yet.  start() happens later, on chatwire's start-up thread,
// once a JVM is known to be present and the mapping mode is known.  stop() runs
// on whichever thread is taking chatwire down.
//
// handle() runs ON THE SOCKET THREAD, one per connected client, and may call
// Java directly from it: chatwire::sdk attaches the calling thread to the VM
// first (see sdk::attach_thread).  It used to have to hand work to a pump and
// answer "queued"; it does not any more, which is why every verb can report
// what actually happened.
//
// NONE of that makes a feature thread-safe.  Two clients are two threads in
// handle() at once, and a feature holding state of its own has to say how that
// is guarded.  It also does not make MINECRAFT thread-safe: the VM permits the
// call, the game may still not, and which game state tolerates it is a
// per-method question — see the notes on send_chat and add_chat in chatwire.sdk for
// the shape of that argument.

export namespace chatwire
{
    /*
        @brief One command from a client, already parsed.
        @details
        `verb` is the part after the LAST dot: a client sending
        `{"cmd":"net.minecraft.world.World.playerEntities"}` reaches the feature
        that claims `net.minecraft.world.World`, with verb "playerEntities".
        Splitting here rather than in each feature keeps the namespacing rule in
        one place.
    */
    struct command
    {
        std::string verb{};
        /* The raw JSON object the client sent, for the feature to read its own
           arguments out of.  Deliberately not pre-parsed into a schema: a
           feature knows its own arguments and a shared schema would have to
           grow every time a feature did. */
        std::string_view body{};
        /*
            Which connection asked.  Zero when nothing did -- the console, a
            test -- so a feature that needs an owner can refuse rather than
            register something in nobody's name.

            Almost no feature wants this: a reply goes back down the socket the
            request arrived on, and the server handles that.  It matters for
            anything that outlives the request, which today means `commands`:
            a registered command belongs to the client that registered it, its
            events go to that client alone, and it is withdrawn when that client
            disconnects.
        */
        std::uint64_t client{ 0 };
    };

    /*
        @brief What a feature sends back.
        @details
        A response is JSON the server writes verbatim.  `ok` false means the
        server should shape it as an error; features do not format their own
        error envelopes, so the wire format stays consistent no matter who
        answered.
    */
    struct response
    {
        bool        ok{ true };
        std::string json_body{};

        static auto success(std::string body = "{}") -> response
        {
            return response{ .ok = true, .json_body = std::move(body) };
        }
        static auto failure(std::string reason) -> response
        {
            return response{ .ok = false, .json_body = std::move(reason) };
        }
    };

    /*
        @brief The interface every subsystem implements.
        @details
        Deliberately tiny.  Four functions is a low enough bar that adding a
        feature is genuinely cheap, which is the property that decides whether
        an extension point gets used or worked around.
    */
    class feature
    {
    public:
        feature() = default;
        virtual ~feature() = default;

        feature(const feature&)                    = delete;
        auto operator=(const feature&) -> feature& = delete;
        feature(feature&&)                         = delete;
        auto operator=(feature&&) -> feature&      = delete;

        /*
            @brief What this feature is called, for the log and for diagnostics.
            @details
            NOT necessarily a command prefix.  "chat" names the chat feature
            everywhere a human reads about it, and is not a command: `chat.*`
            was withdrawn so that every command names the Java member it
            reaches.  What a feature answers to is claims(), below.
        */
        [[nodiscard]] virtual auto name() const noexcept -> std::string_view = 0;

        /*
            @brief Whether this feature answers to `prefix`.
            @details
            Defaults to "my name and nothing else", which is right for a feature
            that reaches nothing in the game and therefore has no Java name to
            take -- `system` is the only one.  Every feature that DOES call into
            Minecraft overrides this to claim the classes it calls, one per
            class, so a command is the fully-qualified member and a reader can
            check it against the game's source.

            A prefix is matched WHOLE.  Commands split at the LAST dot, so the
            prefix is everything before the verb and may itself contain dots.
        */
        [[nodiscard]] virtual auto claims(const std::string_view prefix) const noexcept -> bool
        {
            return prefix == this->name();
        }

        /*
            @brief Installs whatever this feature needs.
            @return false to report the feature unavailable; the rest of
                    chatwire keeps running without it rather than aborting.
        */
        [[nodiscard]] virtual auto start() noexcept -> bool = 0;

        /* @brief Removes everything start() installed. */
        virtual auto stop() noexcept -> void = 0;

        /*
            @brief Answers one client command.  ON THE SOCKET THREAD.
            @details May call Java directly; the sdk attaches the thread first.
        */
        [[nodiscard]] virtual auto handle(const command& cmd) noexcept -> response = 0;
    };

    namespace registry
    {
        namespace detail
        {
            // Function-local static: the registry is populated by static
            // initialisers in other TUs, and a namespace-scope container would
            // be a static-init-order race with them.  This is the standard
            // construct-on-first-use fix.
            //
            // NOT `inline`, and that word is the whole point.  GCC 16.2 emits an
            // inline function's local static into the module's object AND into
            // every consumer's, and `-Wl,--allow-multiple-definition` -- which
            // the link needs anyway -- then hands each consumer its own copy.
            // Duplicated CODE is harmless; duplicated STATE meant the registry
            // was written by one copy and read from another, so every feature
            // registered and none was found.  A non-inline function in a module
            // interface unit is emitted exactly once, in this unit's object,
            // which is what a separate implementation unit used to buy.
            //
            // Deliberately leaked: `new` with no `delete`, so a detour that
            // outlives shutdown cannot walk freed memory.
            [[nodiscard]] auto storage() noexcept -> std::vector<feature*>&
            {
                static auto* const v{ new std::vector<feature*>{} };
                return *v;
            }
        }

        /*
            @brief Adds `f` to the registry.  Called at static-init time.
            @details
            Takes a raw non-owning pointer on purpose: registered features are
            function-local statics that live for the program's lifetime, so
            there is no ownership to transfer and no destruction order to get
            wrong during DLL unload.
        */
        inline auto add(feature* const f) noexcept -> void
        {
            if (!f) { return; }
            try { detail::storage().push_back(f); } catch (...) { }
        }

        [[nodiscard]] inline auto all() noexcept -> const std::vector<feature*>&
        {
            return detail::storage();
        }

        /* @brief The feature owning `name`, or nullptr. */
        [[nodiscard]] inline auto find(const std::string_view name) noexcept -> feature*
        {
            for (feature* const f : detail::storage())
            {
                if (f && f->claims(name)) { return f; }
            }
            return nullptr;
        }

        namespace detail
        {
            /*
                @brief Which features have started.  Same construct-on-first-use
                       reasoning as `storage()` above.
                @details
                Kept so that a feature which could not start can be RETRIED
                without restarting the ones that did -- starting a feature twice
                installs its hook twice.
            */
            [[nodiscard]] auto started() noexcept -> std::vector<feature*>&
            {
                static auto* const v{ new std::vector<feature*>{} };
                return *v;
            }

            [[nodiscard]] inline auto has_started(feature* const f) noexcept -> bool
            {
                const auto& s{ started() };
                return std::ranges::find(s, f) != s.end();
            }
        }

        /*
            @brief Tries to start every feature that is not running yet.
            @details
            One feature failing does not stop the others: a build where the
            inventory mappings went stale should still bridge chat.

            RETRYABLE, and that is the point rather than a nicety.  A feature
            fails to start when a class it hooks is not loaded, and at inject
            time most of Minecraft is not: a user injects at the main menu, where
            `GuiNewChat` does not exist because no chat box has ever been drawn.
            The chat observer therefore could not install, `chat` and `commands`
            never started, and chatwire ran as a bridge that could send a message
            and never report one -- indistinguishable, from outside, from the
            `avq` mapping bug.

            @return how many features started on THIS call.
        */
        inline auto start_pending() noexcept -> std::size_t
        {
            std::size_t started_now{ 0 };
            for (feature* const f : detail::storage())
            {
                if (!f || detail::has_started(f)) { continue; }
                bool ok{ false };
                try { ok = f->start(); } catch (...) { ok = false; }
                if (ok)
                {
                    ++started_now;
                    try { detail::started().push_back(f); } catch (...) { }
                    chatwire::log::info("feature '{}' started", f->name());
                }
            }
            return started_now;
        }

        /* @brief One feature, and whether it is running. */
        struct status_line
        {
            std::string_view name{};
            bool             started{ false };
        };

        /*
            @brief Every feature and whether it started.  For `system.status`.
            @details
            Worth reporting because "started" is not a formality here: a feature
            whose class was not loaded when chatwire arrived comes up LATER, and
            until it does, the thing it provides silently does not happen.  A
            user wondering why no chat is arriving should be able to see that the
            chat feature is not up yet rather than deduce it.
        */
        [[nodiscard]] inline auto status() noexcept -> std::vector<status_line>
        {
            std::vector<status_line> out;
            try
            {
                for (feature* const f : detail::storage())
                {
                    if (!f) { continue; }
                    out.push_back(status_line{ .name = f->name(),
                                               .started = detail::has_started(f) });
                }
            }
            catch (...) { }
            return out;
        }

        /* @brief How many features are still waiting for their classes. */
        [[nodiscard]] inline auto pending() noexcept -> std::size_t
        {
            std::size_t waiting{ 0 };
            for (feature* const f : detail::storage())
            {
                if (f && !detail::has_started(f)) { ++waiting; }
            }
            return waiting;
        }

        /*
            @brief The first start attempt.  Reports what did not come up.
            @details
            Only this one warns.  A retry that fails is the normal state of a
            client sitting on the title screen, and a warning every few seconds
            saying so would be noise rather than news.
        */
        inline auto start_all() noexcept -> std::size_t
        {
            const std::size_t started{ start_pending() };
            for (feature* const f : detail::storage())
            {
                if (f && !detail::has_started(f))
                {
                    chatwire::log::warn("feature '{}' did NOT start yet; retrying as the "
                                        "game loads more of itself", f->name());
                }
            }
            return started;
        }

        /* @brief Stops every feature, in reverse order. */
        inline auto stop_all() noexcept -> void
        {
            auto& features{ detail::storage() };
            for (auto it{ features.rbegin() }; it != features.rend(); ++it)
            {
                if (!*it) { continue; }
                try { (*it)->stop(); } catch (...) { }
            }
        }

        /*
            @brief Registers a feature by constructing it once, at static-init.
            @details
            The one line a new feature writes:

                namespace { const auto registered = chatwire::registry::install<my_feature>(); }

            The instance is a function-local static so it is never destroyed —
            deliberately, because destruction during DLL unload would race the
            socket thread.
        */
        template<typename feature_type>
        [[nodiscard]] inline auto install() noexcept -> bool
        {
            static feature_type instance{};
            add(&instance);
            return true;
        }
    }
}
