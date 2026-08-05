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
//   chat.sendChatMessage -> EntityPlayerSP.sendChatMessage(String)
//                  Goes to the SERVER, exactly as if typed.  A leading '/' runs
//                  a command.  Other players see it.
//   chat.addChatMessage  -> EntityPlayerSP.addChatMessage(IChatComponent)
//                  CLIENT-side only.  Never transmitted.  Nobody else sees it.
//
// The distinction is the single most important thing about this API and the
// easiest to get wrong, so the two are separate verbs rather than a flag -- and
// the verbs carry the game's own method names, so that a reader who knows
// Minecraft's source needs no explanation and one who does not can go and read
// it.  `chat.send` and `chat.add` remain as aliases for the earlier spelling.
//
// ===========================================================================
// THREADING
// ===========================================================================
// handle() runs on a SOCKET thread and calls Java from it, directly.  The sdk
// attaches the calling thread to the VM first, which is what makes that legal —
// see chatwire::sdk::attach_thread.  There is no queue and no tick to wait for,
// so a verb answers with what actually happened rather than with "accepted".
#include "chatwire/common.hpp"

#include "chatwire/feature.hpp"
#include "chatwire/json.hpp"
#include "chatwire/log.hpp"
#include "chatwire/mapping.hpp"
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

    /* The console sink is SEPARATE from the broadcast sink on purpose: watching
       chat in the console is a use on its own, and it must keep working when
       nothing is connected to the socket. */
    using console_sink = void (*)(std::string_view formatted) noexcept;
    inline std::atomic<console_sink> g_console_sink{ nullptr };
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

        /*
            @brief Answers to "chat" and to the Java classes it actually calls.
            @details
            `chat.addChatMessage` and
            `net.minecraft.client.entity.EntityPlayerSP.addChatMessage` are the
            same command.  The long spelling is the point of the short one made
            explicit: the verbs were already named after Minecraft's methods so
            that a reader who knows the source knows what they do, and naming the
            CLASS as well removes the last thing they would have to guess -- which
            of several `addChatMessage` overloads on which type this reaches.

            GuiNewChat is claimed too, because `printChatMessage` is the method
            the observer hooks, so the event and a future command for it agree.

            Deobfuscated names regardless of the mapping in force: this is what a
            client author types, and they should not have to know whether the jar
            they are attached to is MCP, SRG or OBF to write it.
        */
        [[nodiscard]] auto claims(const std::string_view prefix) const noexcept -> bool override
        {
            return prefix == "chat"
                || prefix == "net.minecraft.client.entity.EntityPlayerSP"
                || prefix == "net.minecraft.client.gui.GuiNewChat";
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
            // because they must come down in one pass, after every thread that
            // could be inside one has been joined.  All a feature has to do is
            // stop producing.
        }

        [[nodiscard]] auto handle(const chatwire::command& cmd) noexcept
            -> chatwire::response override
        {
            try
            {
                // The verbs are named after the Minecraft methods they reach:
                // EntityPlayerSP.sendChatMessage and EntityPlayerSP.addChatMessage.
                // Anyone who has read the game's source already knows what these
                // do and, more usefully, knows the difference between them --
                // which "send" and "add" leave you guessing at.  The short forms
                // still work, because they are what the first clients were
                // written against.
                const bool send{ cmd.verb == "sendChatMessage" || cmd.verb == "send" };
                const bool add{ cmd.verb == "addChatMessage" || cmd.verb == "add" };
                if (send || add)
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
                    if (send && text->size() > 100u)
                    {
                        return chatwire::response::failure(
                            "'text' exceeds the 100-character chat limit");
                    }

                    return deliver(*text, send);
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

                return chatwire::response::failure(
                    "unknown verb; try sendChatMessage, addChatMessage or stats");
            }
            catch (...)
            {
                return chatwire::response::failure("internal error");
            }
        }

    private:
        /*
            @brief Receives one chat line, inside a detour.
            @details
            Usually on the game thread, because that is who normally prints
            chat — but not always: `addChatMessage` from a client lands in
            printChatMessage too, so this can also fire on the socket thread that
            asked for it.  Nothing here cares which, and nothing here may start
            caring: the counter is atomic and both sinks are safe from any
            thread.

            noexcept and fully guarded whichever thread it is: the frame above is
            Minecraft's interpreter, which has no handler for a C++ exception.
        */
        static auto on_chat_line(const char* const formatted, const char* const plain) noexcept
            -> void
        {
            try
            {
                g_lines_seen.fetch_add(1, std::memory_order_relaxed);

                const chat_sink    sink{ g_sink.load(std::memory_order_acquire) };
                const console_sink console{ g_console_sink.load(std::memory_order_acquire) };
                if (!sink && !console) { return; }   // nobody watching; do no work

                const std::string_view formatted_view{ formatted ? formatted : "" };
                const std::string_view plain_view{ plain ? plain : "" };
                if (formatted_view.empty() && plain_view.empty()) { return; }

                if (console)
                {
                    console(formatted_view.empty() ? plain_view : formatted_view);
                }
                if (!sink) { return; }

                // Named for the method it comes out of, GuiNewChat.printChatMessage,
                // so the event and the source agree.
                const std::string payload{ chatwire::json::object(
                    chatwire::json::field("type", "printChatMessage") + ","
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
            @brief Says it, right here, on the caller's thread.
            @details
            Synchronous, which is the whole difference from the version that fed
            a pump: by the time this returns the Java call has happened, so the
            reply can say `sent` or `added` and mean it.  A client that got
            `{"ok":true}` back knows the message reached the game rather than
            knowing it reached a queue.

            Not being in a world is a FAILURE rather than a silent drop.  There
            is no chat box on the title screen, and a client that asked to say
            something needs to hear that it did not happen — the queued version
            could only log it, because by then nobody was listening.
        */
        [[nodiscard]] static auto deliver(const std::string& text, const bool to_server) noexcept
            -> chatwire::response
        {
            if (!chatwire::sdk::in_world())
            {
                return chatwire::response::failure("not in a world");
            }

            if (to_server)
            {
                if (!chatwire::sdk::send_chat(text))
                {
                    return chatwire::response::failure("sendChatMessage failed");
                }
                g_sent.fetch_add(1, std::memory_order_relaxed);
                return chatwire::response::success(
                    chatwire::json::object(chatwire::json::field("sent", true)));
            }

            if (!chatwire::sdk::add_chat(text))
            {
                return chatwire::response::failure("addChatMessage failed");
            }
            g_added.fetch_add(1, std::memory_order_relaxed);
            return chatwire::response::success(
                chatwire::json::object(chatwire::json::field("added", true)));
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

    /* @brief Installs the sink that shows chat in chatwire's own console. */
    inline auto set_console_sink(const chatwire::features::console_sink sink) noexcept -> void
    {
        chatwire::features::g_console_sink.store(sink, std::memory_order_release);
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
