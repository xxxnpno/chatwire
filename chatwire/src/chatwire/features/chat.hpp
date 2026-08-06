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
// CLIENTS -> GAME.  Two commands, each spelled as the Java member it reaches:
//
//   net.minecraft.client.entity.EntityPlayerSP.sendChatMessage(String)
//                  Goes to the SERVER, exactly as if typed.  A leading '/' runs
//                  a command.  Other players see it.
//   net.minecraft.client.entity.EntityPlayerSP.addChatMessage(IChatComponent)
//                  CLIENT-side only.  Never transmitted.  Nobody else sees it.
//
// The distinction is the single most important thing about this API and the
// easiest to get wrong, so the two are separate commands rather than a flag --
// and each one carries the game's own class and method name, so that a reader
// who knows Minecraft's source needs no explanation and one who does not can go
// and read it.
//
// There is exactly ONE spelling, and it is the fully-qualified member.  The
// short forms (`chat.sendChatMessage`, `chat.send`, `chat.add`) are gone: a
// short name is a second vocabulary to learn, and unlike the long one it cannot
// be checked against anything.  `chat` survives only as the feature's own name
// in the log.
//
// The counters this feature keeps are NOT a chat command -- there is no
// `stats` method in Minecraft to name one after -- so they are answered by
// `system.stats`, which is where the rest of chatwire's self-reporting lives.
// See stats_json() at the bottom of this file.
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

    /*
        @brief The messages this feature puts on the wire.
        @details
        These structs ARE the wire format -- chatwire/json.hpp writes each one
        by walking its members, so a key on the socket is a member name here and
        the two cannot drift.  Adding a field to an event is adding a member.

        `type` carries its value as a default member initialiser, which is where
        the event's name belongs: it is a property of the event rather than
        something each construction site should be trusted to spell.  The name
        is the method the event comes OUT of, in full, so it reads like the
        commands do and can be checked against Minecraft's source.
    */
    struct print_chat_message_event
    {
        std::string_view type{ "net.minecraft.client.gui.GuiNewChat.printChatMessage" };
        /* With the section-sign colour codes still in. */
        std::string_view formatted{};
        /* The same line with them stripped. */
        std::string_view plain{};
    };

    /* @brief The reply to sendChatMessage: it has already reached the server. */
    struct sent_result { bool sent{ true }; };

    /* @brief The reply to addChatMessage: it is already in the chat box. */
    struct added_result { bool added{ true }; };

    /* @brief This feature's contribution to `system.stats`. */
    struct chat_stats
    {
        std::uint64_t lines_seen{ 0 };
        std::uint64_t sent{ 0 };
        std::uint64_t added{ 0 };
    };

    class chat_feature final : public chatwire::feature
    {
    public:
        [[nodiscard]] auto name() const noexcept -> std::string_view override
        {
            return "chat";
        }

        /*
            @brief Answers to the Java classes it actually calls, and to nothing
                   else.
            @details
            A command names the exact member it reaches, so the class is part of
            it: `net.minecraft.client.entity.EntityPlayerSP.addChatMessage` says
            which of several `addChatMessage` overloads on which type this is.
            "chat" used to be accepted as a short prefix and no longer is,
            because a short name promises nothing -- it cannot be checked against
            Minecraft's source, which is the entire reason the verbs were named
            after methods in the first place.

            GuiNewChat is claimed too, because `printChatMessage` is the method
            the observer hooks, so the event and a future command for it agree.

            Deobfuscated names regardless of the mapping in force: this is what a
            client author types, and they should not have to know whether the jar
            they are attached to is MCP, SRG or OBF to write it.
        */
        [[nodiscard]] auto claims(const std::string_view prefix) const noexcept -> bool override
        {
            return prefix == "net.minecraft.client.entity.EntityPlayerSP"
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
                // The verbs ARE the Minecraft methods they reach:
                // EntityPlayerSP.sendChatMessage and EntityPlayerSP.addChatMessage.
                // Anyone who has read the game's source already knows what these
                // do and, more usefully, knows the difference between them --
                // which "send" and "add" left you guessing at, and which is why
                // those two spellings are no longer accepted.
                const bool send{ cmd.verb == "sendChatMessage" };
                const bool add{ cmd.verb == "addChatMessage" };
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

                // No `stats` here: the counters are chatwire's own bookkeeping,
                // not a method on this class, so naming one after this class
                // would be the one thing these names promise never to do.  They
                // are answered by `system.stats`.
                return chatwire::response::failure(
                    "unknown member; try sendChatMessage or addChatMessage");
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

                const std::string payload{ chatwire::json::object(
                    print_chat_message_event{ .formatted = formatted_view,
                                              .plain     = plain_view }) };

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
                return chatwire::response::success(chatwire::json::object(sent_result{}));
            }

            if (!chatwire::sdk::add_chat(text))
            {
                return chatwire::response::failure("addChatMessage failed");
            }
            g_added.fetch_add(1, std::memory_order_relaxed);
            return chatwire::response::success(chatwire::json::object(added_result{}));
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
        @brief This feature's counters, for `system.stats`.
        @details
        Lives here because the numbers do, and is READ from the system feature
        rather than reachable as a chat command, because there is no Minecraft
        member called `stats` for such a command to be named after.  The host
        wires the two together (see chatwire::features::system::set_stats_source)
        so that neither feature has to include the other.

        Returns the STRUCT, not JSON.  It used to return a brace-less fragment
        of JSON -- text that was valid nowhere on its own -- because the host had
        to merge several features' counters into one object and merging finished
        objects would have meant unpicking their braces.  json::object takes
        several structs and flattens them, so the fragment has no reason to
        exist and neither does the paragraph that used to explain it.

        Safe from any thread: three relaxed loads.  They are not a consistent
        snapshot of each other and do not need to be -- nothing here is a
        difference or a ratio.

        The counters are also no longer cast to std::int64_t on the way out.
        That cast was for the one numeric `field()` overload there was; the
        writer takes the member's own type now, and these are counts that cannot
        go negative.
    */
    [[nodiscard]] inline auto stats() noexcept -> chatwire::features::chat_stats
    {
        return chatwire::features::chat_stats{
            .lines_seen = chatwire::features::g_lines_seen.load(std::memory_order_relaxed),
            .sent       = chatwire::features::g_sent.load(std::memory_order_relaxed),
            .added      = chatwire::features::g_added.load(std::memory_order_relaxed) };
    }

    /*
        @brief This feature's singleton, for the root module to register.
        @details
        A function-local static, constructed on first call and never destroyed.

        Registration is EXPLICIT — src/chatwire.cpp calls this and hands the result
        to the registry — rather than automatic via a namespace-scope
        initialiser.  Two reasons, and the second is the one that matters:

          1. Static-initialisation order across translation units is unspecified,
             and a registry populated by initialisers is a classic source of
             "works until you add the fourth feature" bugs.  Explicit ordering
             cannot have that problem.
          2. GCC 15 ICEs (segfault) on a namespace-scope dynamic initialiser in a
             module interface unit, which is how this was written first.

        Adding a feature is still two lines in src/chatwire.cpp: one include, one
        registry::add.
    */
    [[nodiscard]] inline auto instance() noexcept -> chatwire::feature*
    {
        static chatwire::features::chat_feature feature{};
        return &feature;
    }
}
