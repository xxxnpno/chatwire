// chatwire-mock — chatwire's wire protocol, without Minecraft.
//
// Serves the same WebSocket API the real thing does, but the chat lines are
// synthetic and the commands are acknowledged rather than executed.
//
// It exists because the alternative is developing a consumer with a Minecraft
// client open on the other monitor.  Anything that talks to chatwire — a bot, a
// bridge, an overlay — can be built and tested against this, and the only thing
// it will not catch is a mapping problem inside the game.
//
// It is also how the reference client is tested in CI: both halves of the
// protocol get exercised with no JVM anywhere.
//
// USAGE
//   chatwire-mock                 serve on 24455 and emit a line every 3s
//   chatwire-mock --port 9000     a different port
//   chatwire-mock --interval 500  emit a line every 500 ms
//   chatwire-mock --quiet         only echo commands, never emit chat
#include "chatwire/common.hpp"
#include "chatwire/json.hpp"
#include "chatwire/ws/server.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>


namespace
{
    std::atomic<bool> g_running{ true };

    /*
        @brief Answers a command the way the real dispatcher does.
        @details
        Same envelope, same errors, same `queued` semantics — a consumer cannot
        tell the difference except that nothing reaches a game.  Deliberately
        mirrors chatwire.ixx's dispatch(): if the two drift, this stops being a
        useful stand-in.
    */
    auto handle(const std::string_view request) noexcept -> std::string
    {
        const auto reply{ [](const bool ok, const std::string& body) -> std::string
        {
            try
            {
                return chatwire::json::object(
                    chatwire::json::field("ok", ok) + ","
                    + (ok ? "\"result\":" + body
                          : chatwire::json::field("error", body)));
            }
            catch (...) { return R"({"ok":false,"error":"internal error"})"; }
        } };

        try
        {
            const auto cmd{ chatwire::json::get_string(request, "cmd") };
            if (!cmd) { return reply(false, "missing or non-string 'cmd'"); }

            if (*cmd == "chat.send" || *cmd == "chat.add")
            {
                const auto text{ chatwire::json::get_string(request, "text") };
                if (!text)         { return reply(false, "missing or non-string 'text'"); }
                if (text->empty()) { return reply(false, "'text' is empty"); }
                if (*cmd == "chat.send" && text->size() > 100u)
                {
                    return reply(false, "'text' exceeds the 100-character chat limit");
                }

                std::printf("  \x1b[96m<- %s\x1b[0m %s\n", cmd->c_str(), text->c_str());
                std::fflush(stdout);
                return reply(true, chatwire::json::object(
                    chatwire::json::field("queued", true)));
            }

            if (*cmd == "chat.stats")
            {
                return reply(true, chatwire::json::object(
                    chatwire::json::field("lines_seen", std::int64_t{ 0 }) + ","
                    + chatwire::json::field("sent", std::int64_t{ 0 }) + ","
                    + chatwire::json::field("added", std::int64_t{ 0 })));
            }

            const std::size_t dot{ cmd->find('.') };
            if (dot == std::string::npos)
            {
                return reply(false, "'cmd' must look like feature.verb");
            }
            return reply(false, "no feature named '" + cmd->substr(0, dot) + "'");
        }
        catch (...) { return reply(false, "internal error"); }
    }

    /* Chat lines with colour codes, because a consumer that only ever sees
       plain ASCII will not have handled the § codes it meets in production. */
    constexpr const char* k_lines[]{
        "\xC2\xA7""7[\xC2\xA7""bTeam\xC2\xA7""7] \xC2\xA7""fSteve\xC2\xA7""7: \xC2\xA7""fhello there",
        "\xC2\xA7""eAlex \xC2\xA7""6joined the game",
        "\xC2\xA7""cYou were slain by \xC2\xA7""fa Zombie",
        "\xC2\xA7""a[+] \xC2\xA7""fdiamond x3",
        "\xC2\xA7""9<\xC2\xA7""bNotch\xC2\xA7""9> \xC2\xA7""fmock line with \"quotes\" and \\backslash",
    };

    /* @brief Strips § codes, the way IChatComponent.getUnformattedText does. */
    auto strip(const std::string& text) -> std::string
    {
        std::string out;
        for (std::size_t i{ 0 }; i < text.size(); ++i)
        {
            if (static_cast<unsigned char>(text[i]) == 0xC2u
                && i + 2 < text.size()
                && static_cast<unsigned char>(text[i + 1]) == 0xA7u)
            {
                i += 2;
                continue;
            }
            out += text[i];
        }
        return out;
    }
}

int main(const int argc, char** const argv)
{
    unsigned short port{ 24455 };
    unsigned       interval_ms{ 3000 };
    bool           quiet{ false };

    for (int i{ 1 }; i < argc; ++i)
    {
        const std::string arg{ argv[i] };
        if (arg == "--quiet") { quiet = true; }
        else if (arg == "--port" && i + 1 < argc)
        {
            port = static_cast<unsigned short>(std::strtoul(argv[++i], nullptr, 10));
        }
        else if (arg == "--interval" && i + 1 < argc)
        {
            interval_ms = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10));
        }
        else if (arg == "--help" || arg == "-h")
        {
            std::printf("chatwire-mock [--port N] [--interval MS] [--quiet]\n\n"
                        "Serves chatwire's protocol with synthetic chat, so a consumer\n"
                        "can be developed without a Minecraft client running.\n");
            return 0;
        }
    }

    if (const HANDLE out{ ::GetStdHandle(STD_OUTPUT_HANDLE) }; out != INVALID_HANDLE_VALUE)
    {
        DWORD mode{ 0 };
        if (::GetConsoleMode(out, &mode))
        {
            (void)::SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        }
    }
    (void)::SetConsoleOutputCP(CP_UTF8);

    chatwire::ws::server server;
    if (!server.start(port, &handle))
    {
        std::printf("could not listen on 127.0.0.1:%u - is something already there?\n", port);
        return 1;
    }

    // server.port(), not `port`: the two differ whenever 0 was requested, and
    // printing the REQUEST rather than the RESULT is exactly the confusion this
    // reports around -- a log that says 0 while the server listens elsewhere
    // sends you looking for a bug that is not there.
    std::printf("\x1b[92mchatwire-mock\x1b[0m serving ws://127.0.0.1:%u\n", server.port());
    if (quiet) { std::printf("  (quiet: commands are echoed, no chat is emitted)\n"); }
    else       { std::printf("  emitting a synthetic chat line every %u ms\n", interval_ms); }
    std::printf("  Ctrl+C to stop\n\n");

    std::size_t next{ 0 };
    while (g_running.load(std::memory_order_acquire))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds{ quiet ? 200u : interval_ms });
        if (quiet || server.client_count() == 0u) { continue; }

        const std::string formatted{ k_lines[next++ % std::size(k_lines)] };
        const std::string plain{ strip(formatted) };

        server.broadcast(chatwire::json::object(
            chatwire::json::field("type", "chat") + ","
            + chatwire::json::field("formatted", formatted) + ","
            + chatwire::json::field("plain", plain)));

        std::printf("  \x1b[90m-> %s\x1b[0m\n", plain.c_str());
        std::fflush(stdout);
    }

    server.stop();
    return 0;
}
