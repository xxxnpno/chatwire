// test_server — exercises the WebSocket server end to end, with no JVM.
//
// The server, the framing and the JSON are the parts that can be tested without
// a running Minecraft, and they are also the parts most likely to be wrong: a
// hand-written RFC 6455 implementation is exactly the kind of code that looks
// right and mis-handles a length boundary.
//
// So this starts a real server on a real loopback port, connects a real client
// socket, performs a real handshake, and pushes real frames through it —
// including the boundary sizes and the malformed inputs a hostile client would
// send.  What it cannot test is the Minecraft side, which needs a live game.
#include "chatwire/common.hpp"
#include "chatwire/command_line.hpp"
#include "chatwire/json.hpp"
#include "chatwire/ws/server.hpp"
#include "chatwire/ws/websocket.hpp"

#include "chatwire/net.hpp"

#include <cstdio>


namespace
{
    int failures{ 0 };

    auto check(const char* const name, const bool ok) -> void
    {
        std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
        if (!ok) { ++failures; }
    }

    /* The id the server gave the connection the last request arrived on.  The
       commands feature routes events by this, so a test that never looks at it
       would not notice the server handing out the same one twice. */
    std::atomic<std::uint64_t> last_client{ 0 };

    /* The server's message handler for these tests: echoes what it was sent. */
    auto echo_handler(const std::uint64_t client, const std::string_view request) noexcept
        -> std::string
    {
        try
        {
            last_client.store(client, std::memory_order_release);
            const auto cmd{ chatwire::json::get_string(request, "cmd") };
            if (!cmd) { return R"({"ok":false})"; }
            return chatwire::json::object(chatwire::json::field("echo", *cmd));
        }
        catch (...) { return R"({"ok":false})"; }
    }

    /* A minimal client: connect, handshake, send a text frame, read a reply. */
    class test_client
    {
    public:
        [[nodiscard]] auto connect(const std::uint16_t port) -> bool
        {
            sock_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (sock_ == chatwire::net::invalid_socket) { return false; }
            chatwire::net::prepare(sock_);

            sockaddr_in addr{};
            addr.sin_family      = AF_INET;
            addr.sin_port        = chatwire::net::to_network_port(port);
            addr.sin_addr.s_addr = chatwire::net::loopback_address();
            if (::connect(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
            {
                return false;
            }

            // A fixed key is fine: the handshake hash is a protocol handshake,
            // not a secret, and a deterministic key makes a failure reproducible.
            const std::string request{
                "GET / HTTP/1.1\r\n"
                "Host: 127.0.0.1\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                "Sec-WebSocket-Version: 13\r\n\r\n" };
            if (!write_all(request.data(), request.size())) { return false; }

            std::string response;
            char        chunk[1024]{};
            while (response.find("\r\n\r\n") == std::string::npos)
            {
                const std::ptrdiff_t n{ chatwire::net::recv_some(sock_, chunk,
                                                                 sizeof(chunk)) };
                if (n <= 0) { return false; }
                response.append(chunk, static_cast<std::size_t>(n));
                if (response.size() > 8192u) { return false; }
            }
            handshake_response_ = response;
            return response.find("101") != std::string::npos;
        }

        [[nodiscard]] auto handshake_response() const -> const std::string&
        {
            return handshake_response_;
        }

        /* Sends a MASKED text frame, as RFC 6455 requires of a client. */
        [[nodiscard]] auto send_text(const std::string_view payload) -> bool
        {
            std::vector<std::uint8_t> frame;
            frame.push_back(0x81u);                        // FIN + text

            const std::size_t n{ payload.size() };
            if (n < 126u)
            {
                frame.push_back(static_cast<std::uint8_t>(0x80u | n));
            }
            else if (n <= 0xFFFFu)
            {
                frame.push_back(0x80u | 126u);
                frame.push_back(static_cast<std::uint8_t>((n >> 8) & 0xFFu));
                frame.push_back(static_cast<std::uint8_t>(n & 0xFFu));
            }
            else
            {
                frame.push_back(0x80u | 127u);
                for (int i{ 7 }; i >= 0; --i)
                {
                    frame.push_back(static_cast<std::uint8_t>(
                        (static_cast<std::uint64_t>(n) >> (i * 8)) & 0xFFu));
                }
            }

            const std::array<std::uint8_t, 4> mask{ 0x12u, 0x34u, 0x56u, 0x78u };
            for (const std::uint8_t b : mask) { frame.push_back(b); }
            for (std::size_t i{ 0 }; i < n; ++i)
            {
                frame.push_back(static_cast<std::uint8_t>(payload[i]) ^ mask[i % 4u]);
            }

            return write_all(frame.data(), frame.size());
        }

        /* Sends a frame with the mask bit CLEAR, which a server must reject. */
        [[nodiscard]] auto send_unmasked_text(const std::string_view payload) -> bool
        {
            std::vector<std::uint8_t> frame;
            frame.push_back(0x81u);
            frame.push_back(static_cast<std::uint8_t>(payload.size()));  // no mask bit
            for (const char c : payload) { frame.push_back(static_cast<std::uint8_t>(c)); }
            return write_all(frame.data(), frame.size());
        }

        /* Reads one server frame's payload.  Server frames are never masked. */
        [[nodiscard]] auto read_text(std::string& out) -> bool
        {
            std::uint8_t header[2]{};
            if (!read_exact(header, 2u)) { return false; }

            std::uint64_t length{ static_cast<std::uint64_t>(header[1] & 0x7Fu) };
            if (length == 126u)
            {
                std::uint8_t ext[2]{};
                if (!read_exact(ext, 2u)) { return false; }
                length = (static_cast<std::uint64_t>(ext[0]) << 8) | ext[1];
            }
            else if (length == 127u)
            {
                std::uint8_t ext[8]{};
                if (!read_exact(ext, 8u)) { return false; }
                length = 0u;
                for (const std::uint8_t b : ext) { length = (length << 8) | b; }
            }
            if (length > 32ull * 1024ull * 1024ull) { return false; }

            out.resize(static_cast<std::size_t>(length));
            return length == 0u
                   || read_exact(reinterpret_cast<std::uint8_t*>(out.data()),
                                 static_cast<std::size_t>(length));
        }

        auto close() -> void
        {
            chatwire::net::close_socket(sock_);
            sock_ = chatwire::net::invalid_socket;
        }

        ~test_client() { close(); }

    private:
        [[nodiscard]] auto write_all(const void* const data, std::size_t n) -> bool
        {
            const auto* cursor{ static_cast<const std::uint8_t*>(data) };
            while (n > 0)
            {
                const std::ptrdiff_t written{ chatwire::net::send_some(sock_, cursor, n) };
                if (written <= 0) { return false; }
                cursor += written;
                n      -= static_cast<std::size_t>(written);
            }
            return true;
        }

        [[nodiscard]] auto read_exact(std::uint8_t* const buffer, const std::size_t n) -> bool
        {
            std::size_t got{ 0 };
            while (got < n)
            {
                const std::ptrdiff_t r{ chatwire::net::recv_some(sock_, buffer + got,
                                                                 n - got) };
                if (r <= 0) { return false; }
                got += static_cast<std::size_t>(r);
            }
            return true;
        }

        chatwire::net::socket_t sock_{ chatwire::net::invalid_socket };
        std::string             handshake_response_{};
    };
}

int main()
{
    (void)chatwire::net::startup();

    // ── framing unit checks, no socket needed ───────────────────────────────
    {
        // RFC 6455 §1.3's worked example: this key must produce this accept.
        check("accept_key_matches_rfc6455_example",
              chatwire::ws::accept_key("dGhlIHNhbXBsZSBub25jZQ==")
              == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");

        // The three length forms, at their exact boundaries.  125/126 and
        // 65535/65536 are where a hand-written encoder goes wrong.
        check("encode_len_125_is_inline",
              chatwire::ws::encode(chatwire::ws::opcode::text, std::string(125u, 'x')).size()
              == 2u + 125u);
        check("encode_len_126_uses_16bit",
              chatwire::ws::encode(chatwire::ws::opcode::text, std::string(126u, 'x')).size()
              == 4u + 126u);
        check("encode_len_65535_uses_16bit",
              chatwire::ws::encode(chatwire::ws::opcode::text, std::string(65535u, 'x')).size()
              == 4u + 65535u);
        check("encode_len_65536_uses_64bit",
              chatwire::ws::encode(chatwire::ws::opcode::text, std::string(65536u, 'x')).size()
              == 10u + 65536u);

        // An UNMASKED client frame is a protocol error, not a message.  Getting
        // this wrong is how a hand-written server ends up trusting attacker
        // bytes it never unmasked.
        const std::array<std::uint8_t, 3> unmasked{ 0x81u, 0x01u, 'x' };
        chatwire::ws::frame f{};
        std::size_t         consumed{ 0 };
        check("decode_rejects_unmasked_client_frame",
              chatwire::ws::decode(unmasked, f, consumed)
              == chatwire::ws::decode_status::protocol_error);

        // A reserved bit set means an extension we never negotiated.
        const std::array<std::uint8_t, 7> reserved{ 0xC1u, 0x81u, 0x00u, 0x00u, 0x00u, 0x00u, 'x' };
        check("decode_rejects_reserved_bits",
              chatwire::ws::decode(reserved, f, consumed)
              == chatwire::ws::decode_status::protocol_error);

        // A control frame may not exceed 125 bytes, nor be fragmented.
        const std::array<std::uint8_t, 2> big_ping{ 0x89u, 0xFEu };
        check("decode_rejects_oversized_control_frame",
              chatwire::ws::decode(big_ping, f, consumed)
              == chatwire::ws::decode_status::protocol_error);

        // A partial frame is INCOMPLETE, never a parse of whatever is there.
        const std::array<std::uint8_t, 1> partial{ 0x81u };
        check("decode_reports_incomplete",
              chatwire::ws::decode(partial, f, consumed)
              == chatwire::ws::decode_status::incomplete);
    }

    // ── JSON: the hostile-input cases ──────────────────────────────────────
    {
        check("json_reads_top_level_string",
              chatwire::json::get_string(
                  R"({"cmd":"net.minecraft.client.entity.EntityPlayerSP.sendChatMessage",)"
                  R"("text":"hi"})", "text").value_or("") == "hi");
        check("json_unescapes",
              chatwire::json::get_string(R"({"text":"a\"b\nc"})", "text").value_or("")
              == "a\"b\nc");
        // A key inside a NESTED object must not be mistaken for a top-level one.
        check("json_ignores_nested_keys",
              !chatwire::json::get_string(R"({"a":{"text":"nested"}})", "text").has_value());
        check("json_rejects_unterminated_string",
              !chatwire::json::get_string(R"({"text":"unterminated)", "text").has_value());
        check("json_rejects_trailing_backslash",
              !chatwire::json::get_string(R"({"text":"bad\)", "text").has_value());
        check("json_missing_key_is_nullopt",
              !chatwire::json::get_string(R"({"cmd":"x"})", "text").has_value());
        // Chat text is attacker-controlled; escaping is what stops a quote in a
        // chat line from forging a second JSON field.
        check("json_escapes_quotes_and_controls",
              chatwire::json::escape("a\"b\nc") == "a\\\"b\\nc");
        check("json_preserves_utf8",
              chatwire::json::escape("\xC2\xA7""a") == "\xC2\xA7""a");
    }

        // REGRESSION.  An unconstrained bool overload SWALLOWS string literals:
        // const char* -> bool is a standard conversion while const char* ->
        // string_view is user-defined, so bool wins and field("type","chat")
        // silently emits `"type":true`.  Valid JSON, completely wrong, and
        // invisible until a consumer wonders why a string field is a boolean.
        // It shipped exactly that way for one build.  These pin the resolution.
        check("json_field_literal_is_a_string",
              chatwire::json::field("type", "chat") == R"("type":"chat")");
        check("json_field_bool_is_a_bool",
              chatwire::json::field("ok", true) == R"("ok":true)");
        check("json_field_string_view_is_a_string",
              chatwire::json::field("k", std::string_view{ "v" }) == R"("k":"v")");
        check("json_field_std_string_is_a_string",
              chatwire::json::field("k", std::string{ "v" }) == R"("k":"v")");
        check("json_field_int_is_a_number",
              chatwire::json::field("n", std::int64_t{ 42 }) == R"("n":42)");

    // ── the command line a plugin is handed ────────────────────────────────
    // What a typed line turns into is the whole contract between chatwire and a
    // plugin: get this wrong and either the command never fires or it fires
    // with the wrong arguments.  It is also the only part of that feature that
    // runs without a JVM, which is why it lives in its own header and why it is
    // worth testing rather than assuming.
    {
        namespace cl = chatwire::command_line;

        check("invoked_name_finds_a_bare_command",
              cl::invoked_name("/ping") == "ping");
        check("invoked_name_stops_at_the_first_space",
              cl::invoked_name("/ping alpha beta") == "ping");
        check("invoked_name_ignores_ordinary_chat",
              cl::invoked_name("hello everyone").empty());
        // A line that merely CONTAINS a slash is not a command: only a leading
        // one is Minecraft's command syntax.
        check("invoked_name_ignores_a_mid_line_slash",
              cl::invoked_name("and/or").empty());
        check("invoked_name_ignores_a_lone_slash", cl::invoked_name("/").empty());
        check("invoked_name_ignores_an_empty_line", cl::invoked_name("").empty());

        const auto none{ cl::arguments("/ping") };
        check("arguments_of_a_bare_command_are_empty", none.empty());

        const auto two{ cl::arguments("/ping alpha beta") };
        check("arguments_are_split_on_whitespace",
              two.size() == 2u && two[0] == "alpha" && two[1] == "beta");

        // Runs of spaces and a trailing one are what a person actually types,
        // and each of them is a chance to hand a plugin an empty argument it
        // never asked for.
        const auto messy{ cl::arguments("/ping   alpha \t beta  ") };
        check("arguments_drop_empties_from_runs_and_trailing_space",
              messy.size() == 2u && messy[0] == "alpha" && messy[1] == "beta");

        check("arguments_of_ordinary_chat_are_empty",
              cl::arguments("hello everyone").empty());
        // The command name must never appear as its own first argument -- the
        // classic off-by-one in this shape of parser.
        const auto not_self{ cl::arguments("/ping alpha") };
        check("arguments_exclude_the_command_name",
              not_self.size() == 1u && not_self[0] == "alpha");

        check("normalise_accepts_a_plain_name", cl::normalise("ping") == "ping");
        check("normalise_drops_one_leading_slash", cl::normalise("/ping") == "ping");
        check("normalise_rejects_a_second_slash", cl::normalise("//ping").empty());
        check("normalise_rejects_a_name_with_a_space", cl::normalise("pi ng").empty());
        check("normalise_rejects_an_empty_name", cl::normalise("").empty());
        check("normalise_rejects_a_lone_slash", cl::normalise("/").empty());
        check("normalise_rejects_a_control_character",
              cl::normalise(std::string_view{ "pi\nng" }).empty());
        check("normalise_rejects_an_over_long_name",
              cl::normalise(std::string(cl::max_name_length + 1u, 'a')).empty());
        check("normalise_accepts_the_longest_allowed_name",
              cl::normalise(std::string(cl::max_name_length, 'a')).size()
              == cl::max_name_length);

        // The round trip that matters: whatever normalise() accepted at
        // registration must be exactly what invoked_name() produces when the
        // player types it, or a command registers successfully and then never
        // fires.  This is the pair that has to agree, so it is asserted as a
        // pair rather than as two separate opinions.
        const std::string_view registered{ cl::normalise("/ping") };
        check("registration_and_invocation_agree",
              !registered.empty()
              && cl::invoked_name("/" + std::string{ registered } + " alpha") == registered);
    }

    // ── the server, over a real socket ─────────────────────────────────────
    {
        chatwire::ws::server server;
        // A high port unlikely to collide with anything the developer is running.
        constexpr std::uint16_t port{ 24999 };
        check("server_starts", server.start(port, &echo_handler));

        {
            test_client client;
            check("client_connects_and_handshakes", client.connect(port));
            check("handshake_returns_correct_accept",
                  client.handshake_response().find("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=")
                  != std::string::npos);

            // Give the server a moment to register the client before counting.
            std::this_thread::sleep_for(std::chrono::milliseconds{ 200 });
            check("server_counts_the_client", server.client_count() == 1u);

            check("client_sends_a_frame",
                  client.send_text(R"({"cmd":"net.minecraft.world.World.playerEntities"})"));
            std::string reply;
            check("client_reads_the_reply", client.read_text(reply));
            check("handler_saw_the_command",
                  reply.find("net.minecraft.world.World.playerEntities")
                  != std::string::npos);

            // A payload spanning the 126-byte boundary exercises the extended
            // length path in BOTH directions.
            const std::string long_cmd(200u, 'a');
            check("client_sends_a_long_frame",
                  client.send_text(R"({"cmd":")" + long_cmd + R"("})"));
            std::string long_reply;
            check("client_reads_the_long_reply", client.read_text(long_reply));
            check("long_command_round_trips",
                  long_reply.find(long_cmd) != std::string::npos);

            // Broadcast is the path chat lines take out of the game.
            server.broadcast(
                R"({"type":"net.minecraft.client.gui.GuiNewChat.printChatMessage",)"
                R"("plain":"hello"})");
            std::string pushed;
            check("client_receives_a_broadcast", client.read_text(pushed));
            check("broadcast_payload_intact", pushed.find("hello") != std::string::npos);

            // Targeted delivery: the route a registered command's event takes
            // out of the game, and the reason a connection has an id at all.
            const std::uint64_t id{ last_client.load(std::memory_order_acquire) };
            check("handler_was_told_which_client_asked", id != 0u);
            check("send_to_reaches_that_client",
                  server.send_to(id, R"({"type":"targeted","command":"ping"})"));
            std::string targeted;
            check("client_receives_the_targeted_frame", client.read_text(targeted));
            check("targeted_payload_intact",
                  targeted.find("\"command\":\"ping\"") != std::string::npos);

            // An id nobody holds is a normal "not connected", not an error and
            // not a write into somebody else's socket.
            check("send_to_an_unknown_client_reports_false",
                  !server.send_to(id + 1000u, R"({"type":"targeted"})"));
            last_client.store(id, std::memory_order_release);
        }

        // Same id after the client is gone: still refused.  This is what stops a
        // command registered by a dead plugin from being delivered to whoever
        // connects next.
        std::this_thread::sleep_for(std::chrono::milliseconds{ 300 });
        check("send_to_a_departed_client_reports_false",
              !server.send_to(last_client.load(std::memory_order_acquire),
                              R"({"type":"targeted"})"));

        // The client's destructor closed the socket; the server must notice and
        // reap it rather than broadcasting into a dead handle forever.
        std::this_thread::sleep_for(std::chrono::milliseconds{ 300 });
        server.broadcast(R"({"type":"ping"})");
        std::this_thread::sleep_for(std::chrono::milliseconds{ 200 });
        check("server_reaps_the_disconnected_client", server.client_count() == 0u);

        // A protocol violation must drop that client, not the server.
        {
            test_client rude;
            check("second_client_connects", rude.connect(port));
            std::this_thread::sleep_for(std::chrono::milliseconds{ 150 });
            check("rude_client_sends_unmasked", rude.send_unmasked_text("nope"));
            std::this_thread::sleep_for(std::chrono::milliseconds{ 300 });
            check("server_dropped_the_protocol_violator", server.client_count() == 0u);
            check("server_still_running", server.is_running());
        }

        // stop() must join every thread; if it did not, this would hang rather
        // than fail, which is why it is the last thing tested.
        server.stop();
        check("server_stops_cleanly", !server.is_running());

        // Starting again on the same port proves SO_REUSEADDR and that stop()
        // really released everything.
        chatwire::ws::server restarted;
        check("server_restarts_on_the_same_port", restarted.start(port, &echo_handler));
        restarted.stop();
    }

    chatwire::net::cleanup();
    std::printf("\n%s\n", failures == 0 ? "ALL PASSED" : "FAILURES PRESENT");
    return failures == 0 ? 0 : 1;
}
