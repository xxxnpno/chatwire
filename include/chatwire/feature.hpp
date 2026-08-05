#pragma once

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
//     name()      what its commands are namespaced under  ("chat")
//     start()     install hooks, resolve mappings         (on the game thread)
//     stop()      take them down                          (on the game thread)
//     handle()    respond to one client command
//
// and registers itself with one line at namespace scope.  The server discovers
// it, routes `chat.send` to it, and nothing else in the project knows it exists.
//
// A second feature is literally: new module, implement the interface, register.
//
// ===========================================================================
// LIFECYCLE, AND WHY IT IS ORDERED THIS WAY
// ===========================================================================
// start() runs ON THE GAME THREAD, inside the pump, because installing a vmhook
// detour resolves klasses and methods — JVM work that has the same thread
// requirements as any other JVM work.  Registration happens at static-init time
// (no JVM yet, so it must not touch one); start() happens later, once a JVM is
// known to be present and the mapping mode is known.
//
// handle() runs ON THE SOCKET THREAD.  A feature that needs Java from there
// must go through the pump — that is the whole reason the pump exists, and the
// reason handle() returns a response by value rather than being handed a socket.
#include "chatwire/common.hpp"

#include "chatwire/log.hpp"
namespace chatwire
{
    /*
        @brief One command from a client, already parsed.
        @details
        `verb` is the part after the feature name: a client sending
        `{"cmd":"chat.send", ...}` reaches the "chat" feature with verb "send".
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

        /* @brief The command namespace, e.g. "chat" for `chat.send`. */
        [[nodiscard]] virtual auto name() const noexcept -> std::string_view = 0;

        /*
            @brief Installs whatever this feature needs.  ON THE GAME THREAD.
            @return false to report the feature unavailable; the rest of
                    chatwire keeps running without it rather than aborting.
        */
        [[nodiscard]] virtual auto start() noexcept -> bool = 0;

        /* @brief Removes everything start() installed.  ON THE GAME THREAD. */
        virtual auto stop() noexcept -> void = 0;

        /*
            @brief Answers one client command.  ON THE SOCKET THREAD.
            @details Must not touch Java directly — use chatwire::pump::submit.
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
            inline auto storage() noexcept -> std::vector<feature*>&
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
                if (f && f->name() == name) { return f; }
            }
            return nullptr;
        }

        /*
            @brief Starts every registered feature.  ON THE GAME THREAD.
            @details
            One feature failing does not stop the others: a build where the
            inventory mappings went stale should still bridge chat.  Returns how
            many started.
        */
        inline auto start_all() noexcept -> std::size_t
        {
            std::size_t started{ 0 };
            for (feature* const f : detail::storage())
            {
                if (!f) { continue; }
                bool ok{ false };
                try { ok = f->start(); } catch (...) { ok = false; }
                if (ok)
                {
                    ++started;
                    chatwire::log::info("feature '{}' started", f->name());
                }
                else
                {
                    chatwire::log::warn("feature '{}' did NOT start; continuing without it",
                                        f->name());
                }
            }
            return started;
        }

        /* @brief Stops every feature, in reverse order.  ON THE GAME THREAD. */
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
