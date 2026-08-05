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
//   any text            -> chat.sendChatMessage  (to the SERVER, as if typed)
//   /add <text>         -> chat.addChatMessage   (CLIENT-side only)
//   /stats              -> chat.stats
//   /status             -> system.status
//   /detach             -> system.detach  (UNLOADS chatwire from the game)
//   /quit               -> disconnect (leaves chatwire running)
//
// The default is `send` rather than `add` because that is what a chat client
// does when you type into it.  `add` is the deliberate one, so it gets a prefix.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <winsock2.h>
#include <ws2tcpip.h>

namespace
{
    std::atomic<bool> g_running{ true };
    SOCKET            g_sock{ INVALID_SOCKET };
    bool              g_raw{ false };

    /* ---- a minimal RFC 6455 client -------------------------------------- */

    auto send_all(const SOCKET sock, const char* data, std::size_t n) -> bool
    {
        while (n > 0)
        {
            const int written{ ::send(sock, data, static_cast<int>(n), 0) };
            if (written <= 0) { return false; }
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
    auto send_text(const SOCKET sock, const std::string_view payload) -> bool
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

    auto recv_exact(const SOCKET sock, unsigned char* buffer, const std::size_t n) -> bool
    {
        std::size_t got{ 0 };
        while (got < n)
        {
            const int r{ ::recv(sock, reinterpret_cast<char*>(buffer + got),
                                static_cast<int>(n - got), 0) };
            if (r <= 0) { return false; }
            got += static_cast<std::size_t>(r);
        }
        return true;
    }

    /* @brief Reads one server frame.  Server frames are never masked. */
    auto recv_frame(const SOCKET sock, std::string& out, unsigned char& opcode) -> bool
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

    auto handshake(const SOCKET sock, const unsigned short port) -> bool
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
            const int n{ ::recv(sock, chunk, sizeof(chunk), 0) };
            if (n <= 0) { return false; }
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
                std::printf("%s\n", payload.c_str());
                std::fflush(stdout);
                continue;
            }

            const std::string type{ json_string(payload, "type") };
            // "chat" was the event's name before it was renamed after the method
            // it comes from; still accepted so a new client can talk to a DLL
            // someone has not got round to replacing.
            if (type == "printChatMessage" || type == "chat")
            {
                const std::string formatted{ json_string(payload, "formatted") };
                const std::string plain{ json_string(payload, "plain") };
                std::printf("%s\n", render(formatted.empty() ? plain : formatted).c_str());
            }
            else if (payload.find("\"ok\":false") != std::string::npos)
            {
                std::printf("\x1b[91m  ! %s\x1b[0m\n",
                            json_string(payload, "error").c_str());
            }
            else
            {
                // A result with no shape we recognise: show it rather than hide it.
                std::printf("\x1b[90m  %s\x1b[0m\n", payload.c_str());
            }
            std::fflush(stdout);
        }

        if (g_running.exchange(false, std::memory_order_acq_rel))
        {
            std::printf("\n\x1b[90m  connection closed.  Press enter to exit.\x1b[0m\n");
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
        const HANDLE in{ ::GetStdHandle(STD_INPUT_HANDLE) };
        DWORD        mode{ 0 };
        if (in != INVALID_HANDLE_VALUE && ::GetConsoleMode(in, &mode))
        {
            std::printf("\n  press enter to close.\n");
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
            std::printf("chatwire-client [--port N] [--raw]\n\n"
                        "  <text>        send to the server, as if typed\n"
                        "  /add <text>   show only to this client\n"
                        "  /stats        counters\n"
                        "  /quit         exit\n");
            return 0;
        }
    }

    // Enable ANSI colour on the Windows console; without this the escape codes
    // print as literal garbage on older consoles.
    if (const HANDLE out{ ::GetStdHandle(STD_OUTPUT_HANDLE) }; out != INVALID_HANDLE_VALUE)
    {
        DWORD mode{ 0 };
        if (::GetConsoleMode(out, &mode))
        {
            (void)::SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        }
    }
    // The game sends UTF-8; without this the section signs and any non-ASCII
    // player name print as mojibake.
    (void)::SetConsoleOutputCP(CP_UTF8);

    WSADATA wsa{};
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        std::printf("WSAStartup failed\n");
        return 1;
    }

    g_sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_sock == INVALID_SOCKET) { std::printf("socket() failed\n"); return 1; }

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = ::htons(port);
    addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);

    std::printf("connecting to ws://127.0.0.1:%u ...\n", port);
    if (::connect(g_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        // Ordered by how likely each is, because "could not connect" on its own
        // sends people to check the wrong thing first.
        std::printf("\n\x1b[91m  could not connect to port %u.\x1b[0m\n\n"
                    "  Check, in order:\n"
                    "    1. Minecraft is running and chatwire has been injected\n"
                    "    2. the game's console says \"chatwire ready on ws://127.0.0.1:<port>\"\n"
                    "    3. that port is the one you are dialling - pass --port if it is not\n",
                    port);
        ::closesocket(g_sock);
        ::WSACleanup();
        return wait_before_exit(1);
    }
    if (!handshake(g_sock, port))
    {
        std::printf("\n\x1b[91m  something is listening on port %u, but it is not "
                    "chatwire.\x1b[0m\n  The websocket handshake was refused.\n", port);
        ::closesocket(g_sock);
        ::WSACleanup();
        return wait_before_exit(1);
    }

    std::printf("\x1b[92mconnected.\x1b[0m  type to chat  \x1b[90m|\x1b[0m  "
                "/add client-side  \x1b[90m|\x1b[0m  /status  \x1b[90m|\x1b[0m  "
                "/detach unloads  \x1b[90m|\x1b[0m  /quit\n\n");

    std::thread reader{ &reader_thread };

    std::string line;
    while (g_running.load(std::memory_order_acquire) && std::getline(std::cin, line))
    {
        if (!g_running.load(std::memory_order_acquire)) { break; }
        if (line.empty()) { continue; }

        std::string request;
        if (line == "/quit") { break; }
        else if (line == "/stats")
        {
            request = R"({"cmd":"chat.stats"})";
        }
        else if (line == "/status")
        {
            request = R"({"cmd":"system.status"})";
        }
        else if (line == "/detach")
        {
            // The connection will close as a consequence; that is the point, not
            // a failure, so say so before it happens.
            std::printf("  \x1b[93mrequesting detach - chatwire will unload and this\n"
                        "  connection will close.\x1b[0m\n");
            request = R"({"cmd":"system.detach"})";
        }
        else if (line.rfind("/add ", 0) == 0)
        {
            request = R"({"cmd":"chat.addChatMessage","text":")"
                      + json_escape(line.substr(5)) + R"("})";
        }
        else
        {
            request = R"({"cmd":"chat.sendChatMessage","text":")"
                      + json_escape(line) + R"("})";
        }

        if (!send_text(g_sock, request))
        {
            std::printf("  send failed; the connection is gone.\n");
            break;
        }
    }

    g_running.store(false, std::memory_order_release);
    // shutdown() rather than closesocket(): it wakes the reader out of recv()
    // so the join below cannot hang, and the reader still owns the handle.
    (void)::shutdown(g_sock, SD_BOTH);
    if (reader.joinable()) { reader.join(); }
    ::closesocket(g_sock);
    ::WSACleanup();
    return 0;
}
