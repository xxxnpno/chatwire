#pragma once

// chatwire.features.chat — the chat bridge.
//
// ===========================================================================
// WHAT IT DOES
// ===========================================================================
// GAME -> CLIENTS.  Hooks GuiNewChat.printChatMessage(IChatComponent).  Every
// line that reaches the chat box goes through that one method — server messages,
// client messages, mod output, death messages, join/leave — so one hook catches
// all of it.  The detour reads the component's text and broadcasts it.
//
// CLIENTS -> GAME.  Two verbs:
//
//   chat.send   -> EntityPlayerSP.sendChatMessage(String)
//                  Goes to the SERVER, exactly as if typed.  A leading '/' runs
//                  a command.  Other players see it.
//   chat.add    -> EntityPlayerSP.addChatMessage(IChatComponent)
//                  CLIENT-side only.  Never transmitted.  Nobody else sees it.
//
// The distinction is the single most important thing about this API and the
// easiest to get wrong, so the two are separate verbs rather than a flag.
//
// ===========================================================================
// THREADING
// ===========================================================================
// handle() runs on a SOCKET thread and only enqueues.  The actual Java calls run
// later, inside the pump's detour, on Minecraft's own thread.  Nothing in this
// file calls Java from a socket thread — that would crash the VM.
#include "chatwire/common.hpp"

#include "chatwire/feature.hpp"
#include "chatwire/json.hpp"
#include "chatwire/log.hpp"
#include "chatwire/mapping.hpp"
#include "chatwire/pump.hpp"
#include "chatwire/sdk.hpp"
namespace chatwire::features
{
    namespace map = chatwire::mapping;

    /*
        @brief Where a chat line goes once the detour has read it.
        @details
        A plain function pointer, installed once by the host before start().
        chatwire's server owns the broadcast; this feature must not know about
        sockets, or it could not be tested without one.
    */
    using chat_sink = void (*)(std::string_view json_line) noexcept;

    inline std::atomic<chat_sink> g_sink{ nullptr };
    inline std::atomic<std::uint64_t> g_lines_seen{ 0 };
    inline std::atomic<std::uint64_t> g_sent{ 0 };
    inline std::atomic<std::uint64_t> g_added{ 0 };

    class chat_feature final : public chatwire::feature
    {
    public:
        [[nodiscard]] auto name() const noexcept -> std::string_view override
        {
            return "chat";
        }

        [[nodiscard]] auto start() noexcept -> bool override
        {
            if (!chatwire::sdk::install_chat_observer(&on_chat_line))
            {
                chatwire::log::warn("chat: could not observe incoming chat; "
                                    "sending still works");
                return false;
            }
            return true;
        }

        auto stop() noexcept -> void override
        {
            // Hooks are removed centrally by chatwire::stop() -> sdk::remove_hooks(),
            // because they must come down in one pass on the game thread.  All a
            // feature has to do is stop producing.
        }

        [[nodiscard]] auto handle(const chatwire::command& cmd) noexcept
            -> chatwire::response override
        {
            try
            {
                if (cmd.verb == "send" || cmd.verb == "add")
                {
                    auto text{ chatwire::json::get_string(cmd.body, "text") };
                    if (!text)
                    {
                        return chatwire::response::failure("missing or non-string 'text'");
                    }
                    if (text->empty())
                    {
                        return chatwire::response::failure("'text' is empty");
                    }
                    // Minecraft 1.8.9 refuses chat longer than 100 characters and
                    // kicks the player for trying, so refusing here is friendlier
                    // than letting the server do it.
                    if (cmd.verb == "send" && text->size() > 100u)
                    {
                        return chatwire::response::failure(
                            "'text' exceeds the 100-character chat limit");
                    }

                    const bool to_server{ cmd.verb == "send" };
                    const bool queued{ chatwire::pump::submit(
                        [message = *std::move(text), to_server]() noexcept
                        {
                            deliver(message, to_server);
                        }) };

                    if (!queued)
                    {
                        return chatwire::response::failure(
                            "the game-thread queue is full or closed");
                    }
                    return chatwire::response::success(
                        chatwire::json::object(chatwire::json::field("queued", true)));
                }

                if (cmd.verb == "stats")
                {
                    return chatwire::response::success(chatwire::json::object(
                        chatwire::json::field("lines_seen",
                            static_cast<std::int64_t>(g_lines_seen.load(std::memory_order_relaxed)))
                        + "," + chatwire::json::field("sent",
                            static_cast<std::int64_t>(g_sent.load(std::memory_order_relaxed)))
                        + "," + chatwire::json::field("added",
                            static_cast<std::int64_t>(g_added.load(std::memory_order_relaxed)))));
                }

                return chatwire::response::failure("unknown verb; try send, add or stats");
            }
            catch (...)
            {
                return chatwire::response::failure("internal error");
            }
        }

    private:
        /*
            @brief Receives one chat line.  ON THE GAME THREAD, in a detour.
            @details
            noexcept and fully guarded: the frame above is Minecraft's
            interpreter, which has no handler for a C++ exception.
        */
        static auto on_chat_line(const char* const formatted, const char* const plain) noexcept
            -> void
        {
            try
            {
                g_lines_seen.fetch_add(1, std::memory_order_relaxed);

                const chat_sink sink{ g_sink.load(std::memory_order_acquire) };
                if (!sink) { return; }              // nobody listening; do no work

                const std::string_view formatted_view{ formatted ? formatted : "" };
                const std::string_view plain_view{ plain ? plain : "" };
                if (formatted_view.empty() && plain_view.empty()) { return; }

                const std::string payload{ chatwire::json::object(
                    chatwire::json::field("type", "chat") + ","
                    + chatwire::json::field("formatted", formatted_view) + ","
                    + chatwire::json::field("plain", plain_view)) };

                sink(payload);
            }
            catch (...)
            {
                // Never let anything reach the interpreter frame above.
            }
        }

        /*
            @brief Performs one queued chat action.  ON THE GAME THREAD.
        */
        static auto deliver(const std::string& text, const bool to_server) noexcept -> void
        {
            if (!chatwire::sdk::in_world())
            {
                chatwire::log::warn("chat: not in a world; dropping message");
                return;
            }

            if (to_server)
            {
                if (chatwire::sdk::send_chat(text))
                {
                    g_sent.fetch_add(1, std::memory_order_relaxed);
                }
                else
                {
                    chatwire::log::warn("chat: sendChatMessage failed");
                }
                return;
            }

            if (chatwire::sdk::add_chat(text))
            {
                g_added.fetch_add(1, std::memory_order_relaxed);
            }
            else
            {
                chatwire::log::warn("chat: addChatMessage failed");
            }
        }

    };
}

namespace chatwire::features::chat
{
    /* @brief Installs the sink every observed chat line is handed to. */
    inline auto set_sink(const chatwire::features::chat_sink sink) noexcept -> void
    {
        chatwire::features::g_sink.store(sink, std::memory_order_release);
    }

    /*
        @brief This feature's singleton, for the root module to register.
        @details
        A function-local static, constructed on first call and never destroyed.

        Registration is EXPLICIT — chatwire.ixx calls this and hands the result
        to the registry — rather than automatic via a namespace-scope
        initialiser.  Two reasons, and the second is the one that matters:

          1. Static-initialisation order across translation units is unspecified,
             and a registry populated by initialisers is a classic source of
             "works until you add the fourth feature" bugs.  Explicit ordering
             cannot have that problem.
          2. GCC 15 ICEs (segfault) on a namespace-scope dynamic initialiser in a
             module interface unit, which is how this was written first.

        Adding a feature is still two lines in chatwire.ixx: one import, one
        registry::add.
    */
    [[nodiscard]] inline auto instance() noexcept -> chatwire::feature*
    {
        static chatwire::features::chat_feature feature{};
        return &feature;
    }
}
