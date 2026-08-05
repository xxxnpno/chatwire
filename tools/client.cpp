// chatwire-client — a terminal WebSocket client for trying chatwire out.
//
// Connects, prints every chat line the game produces, and sends whatever you
// type.  It is the reference consumer: if this works, the API works.
//
// USAGE
//   chatwire-client                  connect to ws://127.0.0.1:24455
//   chatwire-client --port 9000      a different port
//   chatwire-client --raw            print the raw JSON instead of the text
//
// AT THE PROMPT
//   any text
//       -> EntityPlayerSP.sendChatMessage: to the SERVER, exactly as typed
//   /net.minecraft.client.entity.EntityPlayerSP.addChatMessage <text>
//       -> client-side only, nobody else sees it
//   /net.minecraft.world.World.playerEntities
//       -> every player this client has loaded, with name and UUID
//   /system.status                -> version, mapping, port, clients
//   /system.stats                 -> counters
//   /system.ping                  -> liveness
//   /system.detach                -> stops chatwire (it stays loaded)
//   /quit                         -> disconnect, leaving chatwire running
//
// The commands ARE the protocol verbs, typed exactly as they go on the wire,
// and any `/<prefix>.<member> [text]` is sent through untranslated -- so a
// feature added to chatwire is reachable from here without touching this file.
//
// They are long, and that is the protocol's decision rather than this file's: a
// command names the Java member it reaches, so you can check what you are
// getting against Minecraft's source.  This client keeps NO short aliases of its
// own -- /add, /stats, /status and the rest are gone, along with the `chat.` and
// `world.` prefixes the server used to accept.  A reference client with a
// private vocabulary teaches you its vocabulary instead of the API's.
//
// The default is `send` rather than `add` because that is what a chat client
// does when you type into it.  `add` is the deliberate one, so it gets a prefix.
#include <print>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "chatwire/net.hpp"
#include "chatwire/terminal.hpp"

namespace
{
    std::atomic<bool>       g_running{ true };
    chatwire::net::socket_t g_sock{ chatwire::net::invalid_socket };
    bool                    g_raw{ false };

    // The protocol's names, spelled once.  Written out in full at the prompt
    // too: this client does not translate, so what you type here is what a tool
    // in any other language would put in its `cmd` field.
    constexpr const char* k_send{
        "net.minecraft.client.entity.EntityPlayerSP.sendChatMessage" };
    constexpr const char* k_add{
        "net.minecraft.client.entity.EntityPlayerSP.addChatMessage" };
    constexpr const char* k_players{ "net.minecraft.world.World.playerEntities" };
    constexpr const char* k_chat_event{
        "net.minecraft.client.gui.GuiNewChat.printChatMessage" };

    /* ---- a minimal RFC 6455 client -------------------------------------- */

    auto send_all(const chatwire::net::socket_t sock, const char* data, std::size_t n) -> bool
    {
        while (n > 0)
        {
            const std::ptrdiff_t written{ chatwire::net::send_some(sock, data, n) };
            if (written <= 0)
            {
                if (written < 0 && chatwire::net::retryable(chatwire::net::last_error()))
                {
                    continue;
                }
                return false;
            }
            data += written;
            n    -= static_cast<std::size_t>(written);
        }
        return true;
    }

    /*
        @brief Sends a masked text frame.
        @details
        A client MUST mask (RFC 6455 §5.1) and a compliant server drops it if it
        does not.  The mask is not a security measure — it exists so a hostile
        page cannot make a proxy see attacker-chosen plaintext — so a fixed mask
        would technically work, but a varying one is what the RFC intends.
    */
    auto send_text(const chatwire::net::socket_t sock, const std::string_view payload) -> bool
    {
        std::vector<unsigned char> frame;
        frame.push_back(0x81u);                          // FIN + text

        const std::size_t n{ payload.size() };
        if (n < 126u)
        {
            frame.push_back(static_cast<unsigned char>(0x80u | n));
        }
        else if (n <= 0xFFFFu)
        {
            frame.push_back(0x80u | 126u);
            frame.push_back(static_cast<unsigned char>((n >> 8) & 0xFFu));
            frame.push_back(static_cast<unsigned char>(n & 0xFFu));
        }
        else
        {
            frame.push_back(0x80u | 127u);
            for (int i{ 7 }; i >= 0; --i)
            {
                frame.push_back(static_cast<unsigned char>(
                    (static_cast<unsigned long long>(n) >> (i * 8)) & 0xFFu));
            }
        }

        static unsigned char counter{ 0 };
        const unsigned char  mask[4]{ static_cast<unsigned char>(0x21u + counter++),
                                      0x5Au, 0xA5u, 0x3Cu };
        for (const unsigned char b : mask) { frame.push_back(b); }
        for (std::size_t i{ 0 }; i < n; ++i)
        {
            frame.push_back(static_cast<unsigned char>(payload[i]) ^ mask[i % 4u]);
        }

        return send_all(sock, reinterpret_cast<const char*>(frame.data()), frame.size());
    }

    auto recv_exact(const chatwire::net::socket_t sock, unsigned char* buffer, const std::size_t n) -> bool
    {
        std::size_t got{ 0 };
        while (got < n)
        {
            const std::ptrdiff_t r{ chatwire::net::recv_some(sock, buffer + got, n - got) };
            if (r <= 0)
            {
                if (r < 0 && chatwire::net::retryable(chatwire::net::last_error()))
                {
                    continue;
                }
                return false;
            }
            got += static_cast<std::size_t>(r);
        }
        return true;
    }

    /* @brief Reads one server frame.  Server frames are never masked. */
    auto recv_frame(const chatwire::net::socket_t sock, std::string& out, unsigned char& opcode) -> bool
    {
        unsigned char header[2]{};
        if (!recv_exact(sock, header, 2u)) { return false; }
        opcode = static_cast<unsigned char>(header[0] & 0x0Fu);

        unsigned long long length{ static_cast<unsigned long long>(header[1] & 0x7Fu) };
        if (length == 126u)
        {
            unsigned char ext[2]{};
            if (!recv_exact(sock, ext, 2u)) { return false; }
            length = (static_cast<unsigned long long>(ext[0]) << 8) | ext[1];
        }
        else if (length == 127u)
        {
            unsigned char ext[8]{};
            if (!recv_exact(sock, ext, 8u)) { return false; }
            length = 0u;
            for (const unsigned char b : ext) { length = (length << 8) | b; }
        }
        if (length > 32ull * 1024ull * 1024ull) { return false; }

        out.resize(static_cast<std::size_t>(length));
        return length == 0u
               || recv_exact(sock, reinterpret_cast<unsigned char*>(out.data()),
                             static_cast<std::size_t>(length));
    }

    auto handshake(const chatwire::net::socket_t sock, const unsigned short port) -> bool
    {
        const std::string request{
            "GET / HTTP/1.1\r\n"
            "Host: 127.0.0.1:" + std::to_string(port) + "\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: Y2hhdHdpcmUtY2xpZW50AAA=\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n" };
        if (!send_all(sock, request.data(), request.size())) { return false; }

        std::string response;
        char        chunk[1024]{};
        while (response.find("\r\n\r\n") == std::string::npos)
        {
            const std::ptrdiff_t n{ chatwire::net::recv_some(sock, chunk, sizeof(chunk)) };
            if (n <= 0)
            {
                if (n < 0 && chatwire::net::retryable(chatwire::net::last_error()))
                {
                    continue;
                }
                return false;
            }
            response.append(chunk, static_cast<std::size_t>(n));
            if (response.size() > 16384u) { return false; }
        }
        return response.find(" 101 ") != std::string::npos;
    }

    /* ---- just enough JSON to read a flat object ------------------------- */

    auto json_string(const std::string& object, const std::string& key) -> std::string
    {
        const std::string needle{ "\"" + key + "\":\"" };
        const std::size_t at{ object.find(needle) };
        if (at == std::string::npos) { return {}; }

        std::string out;
        for (std::size_t i{ at + needle.size() }; i < object.size(); ++i)
        {
            const char c{ object[i] };
            if (c == '"') { break; }
            if (c == '\\' && i + 1 < object.size())
            {
                const char esc{ object[++i] };
                switch (esc)
                {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': break;
                default:  out += esc;  break;
                }
                continue;
            }
            out += c;
        }
        return out;
    }

    /*
        @brief Renders Minecraft's section-sign colour codes as ANSI colour.
        @details
        The point of a reference client is to show the API is usable, and chat
        without colour looks broken to anyone who knows Minecraft.  Unmapped
        codes (obfuscated, strikethrough) are simply dropped rather than
        approximated.
    */
    auto render(const std::string& text) -> std::string
    {
        static const char* const palette[16]{
            "\x1b[30m", "\x1b[34m", "\x1b[32m", "\x1b[36m",
            "\x1b[31m", "\x1b[35m", "\x1b[33m", "\x1b[37m",
            "\x1b[90m", "\x1b[94m", "\x1b[92m", "\x1b[96m",
            "\x1b[91m", "\x1b[95m", "\x1b[93m", "\x1b[97m" };

        std::string out;
        for (std::size_t i{ 0 }; i < text.size(); ++i)
        {
            // The section sign is U+00A7 = 0xC2 0xA7 in UTF-8.
            const bool is_section{ static_cast<unsigned char>(text[i]) == 0xC2u
                                   && i + 1 < text.size()
                                   && static_cast<unsigned char>(text[i + 1]) == 0xA7u };
            if (!is_section) { out += text[i]; continue; }
            if (i + 2 >= text.size()) { break; }

            const char code{ text[i + 2] };
            i += 2;
            if (code >= '0' && code <= '9')      { out += palette[code - '0']; }
            else if (code >= 'a' && code <= 'f') { out += palette[code - 'a' + 10]; }
            else if (code == 'l')                { out += "\x1b[1m"; }
            else if (code == 'o')                { out += "\x1b[3m"; }
            else if (code == 'n')                { out += "\x1b[4m"; }
            else if (code == 'r')                { out += "\x1b[0m"; }
            // k (obfuscated) and m (strikethrough) are dropped.
        }
        out += "\x1b[0m";
        return out;
    }

    /*
        @brief Prints a playerEntities result as a list rather than as JSON.
        @details
        The reader does not know which command a reply belongs to -- the
        protocol has no correlation id and does not need one for a client with
        one prompt -- so this recognises the ANSWER instead: a `players` array
        is a shape only that command produces.

        Entries are flat objects of two string fields, so '}' ends one and ']'
        ends the list.  That is a parser this reference client can afford to be
        honest about: it is showing that the API is usable, not shipping a JSON
        library, and `--raw` is right there for anyone who wants the bytes.

        @return false when this payload is not a player list, so the caller can
                fall through to its other cases.
    */
    auto render_players(const std::string& payload) -> bool
    {
        const std::string needle{ "\"players\":[" };
        const std::size_t at{ payload.find(needle) };
        if (at == std::string::npos) { return false; }

        std::size_t shown{ 0 };
        for (std::size_t i{ at + needle.size() }; i < payload.size(); ++i)
        {
            if (payload[i] == ']') { break; }
            if (payload[i] != '{') { continue; }

            const std::size_t end{ payload.find('}', i) };
            if (end == std::string::npos) { break; }

            const std::string entry{ payload.substr(i, end - i + 1u) };
            i = end;

            std::println("  \x1b[97m{:<17}\x1b[0m \x1b[90m{}\x1b[0m",
                        json_string(entry, "name"), json_string(entry, "uuid"));
            ++shown;
        }

        // Said every time, including for 0: "the client has loaded none" and
        // "the command did nothing" look identical otherwise.  This is the
        // client's entity list, not the server's roster -- on a big server it
        // is a small fraction of the tab list, and a count with no explanation
        // is exactly how that gets mistaken for a bug.
        std::println("\x1b[90m  {} player(s) loaded by this client\x1b[0m", shown);
        return true;
    }

    auto reader_thread() -> void
    {
        std::string   payload;
        unsigned char opcode{ 0 };

        while (g_running.load(std::memory_order_acquire))
        {
            if (!recv_frame(g_sock, payload, opcode)) { break; }

            if (opcode == 0x8u) { break; }                       // close
            if (opcode == 0x9u) { continue; }                    // ping; server-side only
            if (payload.empty()) { continue; }

            if (g_raw)
            {
                std::println("{}", payload);
                std::fflush(stdout);
                continue;
            }

            const std::string type{ json_string(payload, "type") };
            // The event is named after the method it comes out of, in full, the
            // same way the commands are.  The earlier short spellings ("chat",
            // then "printChatMessage") are NOT accepted: this client is the
            // reference for one vocabulary, and quietly understanding two would
            // hide a version mismatch that the protocol should report.
            if (type == k_chat_event)
            {
                const std::string formatted{ json_string(payload, "formatted") };
                const std::string plain{ json_string(payload, "plain") };
                std::println("{}", render(formatted.empty() ? plain : formatted));
            }
            else if (payload.find("\"ok\":false") != std::string::npos)
            {
                std::println("\x1b[91m  ! {}\x1b[0m", json_string(payload, "error"));
            }
            else if (render_players(payload))
            {
                // Already printed as a list.
            }
            else
            {
                // A result with no shape we recognise: show it rather than hide it.
                std::println("\x1b[90m  {}\x1b[0m", payload);
            }
            std::fflush(stdout);
        }

        if (g_running.exchange(false, std::memory_order_acq_rel))
        {
            std::println("\n\x1b[90m  connection closed.  Press enter to exit.\x1b[0m");
            std::fflush(stdout);
        }
    }

    /*
        @brief Keeps the window open when there is nothing else to read.
        @details
        A double-clicked console exe closes the instant main() returns, so an
        error message that took two seconds to appear is gone before it can be
        read -- the failure looks like "it just closes" rather than like the
        diagnosis it actually printed.  Only pauses when stdin is a terminal, so
        piping into this from a script still exits immediately.
    */
    auto wait_before_exit(const int code) -> int
    {
        if (chatwire::terminal::stdin_is_interactive())
        {
            std::println("\n  press enter to close.");
            (void)std::getchar();
        }
        return code;
    }


    auto json_escape(const std::string& text) -> std::string
    {
        std::string out;
        for (const char c : text)
        {
            switch (c)
            {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20u) { break; }
                out += c;
                break;
            }
        }
        return out;
    }
}

int main(const int argc, char** const argv)
{
    unsigned short port{ 24455 };
    for (int i{ 1 }; i < argc; ++i)
    {
        const std::string arg{ argv[i] };
        if (arg == "--raw") { g_raw = true; }
        else if (arg == "--port" && i + 1 < argc)
        {
            port = static_cast<unsigned short>(std::strtoul(argv[++i], nullptr, 10));
        }
        else if (arg == "--help" || arg == "-h")
        {
            std::println("chatwire-client [--port N] [--raw]\n\n"
                        "  <text>            send to the server, as if typed\n"
                        "  /{} <text>\n"
                        "                    show only to this client\n"
                        "  /{}\n"
                        "                    every player this client has loaded\n"
                        "  /system.status    version, mapping, port, clients\n"
                        "  /system.stats     counters\n"
                        "  /system.ping      liveness\n"
                        "  /system.detach    stop chatwire (it stays loaded)\n"
                        "  /quit             exit\n\n"
                        "Commands are the protocol's own, typed in full: each one names the\n"
                        "Java member it reaches.  There are no short aliases.",
                        k_add, k_players);
            return 0;
        }
    }

    // Colour and UTF-8.  Nothing to do off Windows, where a terminal has
    // understood both for decades; on Windows, without this the escape codes
    // print as literal garbage and every section sign arrives as mojibake.
    chatwire::terminal::enable_ansi();

    if (!chatwire::net::startup())
    {
        std::println("could not initialise the socket library");
        return 1;
    }

    g_sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_sock == chatwire::net::invalid_socket)
    {
        std::println("socket() failed");
        return 1;
    }
    // Suppresses SIGPIPE on macOS: writing to a socket chatwire has closed would
    // otherwise kill this process outright instead of returning an error, which
    // is exactly what happens on the /system.detach path.
    chatwire::net::prepare(g_sock);

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = ::htons(port);
    addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);

    std::println("connecting to ws://127.0.0.1:{} ...", port);
    if (::connect(g_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        // Ordered by how likely each is, because "could not connect" on its own
        // sends people to check the wrong thing first.
        std::println("\n\x1b[91m  could not connect to port {}.\x1b[0m\n\n"
                    "  Check, in order:\n"
                    "    1. Minecraft is running and chatwire has been injected\n"
                    "    2. the game's console says \"chatwire ready on ws://127.0.0.1:<port>\"\n"
                    "    3. that port is the one you are dialling - pass --port if it is not", port);
        chatwire::net::close_socket(g_sock);
        chatwire::net::cleanup();
        return wait_before_exit(1);
    }
    if (!handshake(g_sock, port))
    {
        std::println("\n\x1b[91m  something is listening on port {}, but it is not "
                    "chatwire.\x1b[0m\n  The websocket handshake was refused.", port);
        chatwire::net::close_socket(g_sock);
        chatwire::net::cleanup();
        return wait_before_exit(1);
    }

    // The commands ARE the protocol verbs.  This is the reference consumer, so
    // what it shows you is what you would put in a `cmd` field yourself -- a
    // client with its own private vocabulary teaches you its vocabulary instead
    // of the API's.
    std::println("\x1b[92mconnected.\x1b[0m  anything you type goes to\n"
                "  \x1b[96m{}\x1b[0m\n\n"
                "  \x1b[96m/{} \x1b[90m<text>\x1b[0m\n"
                "      \x1b[90mclient-side only; nobody else sees it\x1b[0m\n"
                "  \x1b[96m/{}\x1b[0m\n"
                "      \x1b[90mevery player this client has loaded, with name and UUID\x1b[0m\n"
                "  \x1b[96m/system.status\x1b[0m  "
                "\x1b[96m/system.stats\x1b[0m  "
                "\x1b[96m/system.ping\x1b[0m  "
                "\x1b[96m/system.detach\x1b[0m  "
                "\x1b[90m/quit\x1b[0m\n",
                k_send, k_add, k_players);

    std::thread reader{ &reader_thread };

    std::string line;
    while (g_running.load(std::memory_order_acquire) && std::getline(std::cin, line))
    {
        if (!g_running.load(std::memory_order_acquire)) { break; }
        if (line.empty()) { continue; }

        std::string request;
        if (line == "/quit") { break; }
        else if (line[0] == '/')
        {
            // Everything after the slash is passed through UNINTERPRETED as
            // <prefix>.<member>, with the rest of the line as `text`.  There are
            // no aliases to rewrite: /add, /stats, /status, /ping and /detach
            // are gone, and so is every short server prefix they expanded to.
            // That is what makes this a reference client rather than a menu --
            // a feature added to chatwire tomorrow is reachable from here today,
            // with no change to this file, and what you type is what a tool in
            // any other language would send.
            const std::string command{ line.substr(1) };
            const std::size_t space{ command.find(' ') };
            const std::string verb{ command.substr(0, space) };
            const std::string text{ space == std::string::npos
                                        ? std::string{}
                                        : command.substr(space + 1) };

            if (verb.find('.') == std::string::npos)
            {
                std::println("  \x1b[91mnot a command:\x1b[0m {}\n"
                            "  \x1b[90m(commands look like <class>.<member>, e.g. {})\x1b[0m",
                            verb, k_players);
                continue;
            }

            if (verb == "system.detach")
            {
                // The connection closing is the point, not a failure, so say so
                // before it happens.  chatwire stops but stays loaded in the
                // game -- unloading it while a game thread might be inside it is
                // what used to kill the process.
                std::println("  \x1b[93mrequesting detach - chatwire will stop and this\n"
                            "  connection will close.  Re-run chatwire-inject to "
                            "start it again.\x1b[0m");
            }

            request = R"({"cmd":")" + json_escape(verb) + R"("})";
            if (!text.empty())
            {
                request = R"({"cmd":")" + json_escape(verb) + R"(","text":")"
                          + json_escape(text) + R"("})";
            }
        }
        else
        {
            request = R"({"cmd":")" + std::string{ k_send } + R"(","text":")"
                      + json_escape(line) + R"("})";
        }

        if (!send_text(g_sock, request))
        {
            std::println("  send failed; the connection is gone.");
            break;
        }
    }

    g_running.store(false, std::memory_order_release);
    // shutdown() rather than close(): it wakes the reader out of recv() on all
    // three platforms, so the join below cannot hang, and the descriptor stays
    // valid until after that join rather than being handed back to the OS while
    // another thread is still blocked on it.
    chatwire::net::shutdown_both(g_sock);
    if (reader.joinable()) { reader.join(); }
    chatwire::net::close_socket(g_sock);
    chatwire::net::cleanup();
    return 0;
}
