#pragma once

// chatwire.ws.websocket — RFC 6455 handshake and framing, by hand.
//
// ===========================================================================
// WHY HAND-WRITTEN
// ===========================================================================
// This code runs INSIDE someone else's JVM process, injected.  Every library it
// pulls in is a library that has to agree with whatever the host process
// already loaded, and a CRT or OpenSSL mismatch inside a foreign process is a
// crash with no useful stack.  The server is also local-only and unencrypted by
// design (see ws/server.hpp), so the parts of a real WebSocket library that earn
// their weight — TLS, permessage-deflate, proxy handling — are exactly the
// parts this does not want.
//
// What is left is small: a base64 SHA-1 handshake and a frame codec.  Both are
// below, both are self-contained, and the only dependency is ws2_32.
//
// ===========================================================================
// SCOPE, STATED HONESTLY
// ===========================================================================
// Implemented: the server half of RFC 6455 for text and close frames, client
// masking (mandatory, and clients that omit it are dropped), fragmentation
// reassembly, ping/pong, and the 125-byte control-frame limit.
//
// NOT implemented: extensions (the handshake never negotiates one, so a
// compliant client will not send one), binary frames beyond passing the opcode
// through, and continuation of control frames (which RFC 6455 forbids anyway).
#include "chatwire/common.hpp"
namespace chatwire::ws::detail
{
    // ── SHA-1 ───────────────────────────────────────────────────────────────
    // Required by the handshake and by nothing else.  RFC 3174, ~40 lines, and
    // vastly less trouble than linking a crypto library into a foreign process.
    // No secrecy claim is being made here: the handshake hash is a protocol
    // handshake, not a security measure.
    struct sha1
    {
        std::array<std::uint32_t, 5> state{ 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu,
                                            0x10325476u, 0xC3D2E1F0u };
        std::array<std::uint8_t, 64> block{};
        std::uint64_t                length{ 0 };
        std::size_t                  used{ 0 };

        static constexpr auto rol(const std::uint32_t v, const int n) noexcept
            -> std::uint32_t
        {
            return (v << n) | (v >> (32 - n));
        }

        auto compress() noexcept -> void
        {
            std::array<std::uint32_t, 80> w{};
            for (std::size_t i{ 0 }; i < 16; ++i)
            {
                w[i] = (static_cast<std::uint32_t>(block[i * 4 + 0]) << 24)
                     | (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16)
                     | (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8)
                     |  static_cast<std::uint32_t>(block[i * 4 + 3]);
            }
            for (std::size_t i{ 16 }; i < 80; ++i)
            {
                w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
            }

            std::uint32_t a{ state[0] }, b{ state[1] }, c{ state[2] },
                          d{ state[3] }, e{ state[4] };
            for (std::size_t i{ 0 }; i < 80; ++i)
            {
                std::uint32_t f{ 0 };
                std::uint32_t k{ 0 };
                if (i < 20)      { f = (b & c) | (~b & d);            k = 0x5A827999u; }
                else if (i < 40) { f = b ^ c ^ d;                     k = 0x6ED9EBA1u; }
                else if (i < 60) { f = (b & c) | (b & d) | (c & d);   k = 0x8F1BBCDCu; }
                else             { f = b ^ c ^ d;                     k = 0xCA62C1D6u; }

                const std::uint32_t tmp{ rol(a, 5) + f + e + k + w[i] };
                e = d; d = c; c = rol(b, 30); b = a; a = tmp;
            }
            state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
        }

        auto update(const std::span<const std::uint8_t> data) noexcept -> void
        {
            length += static_cast<std::uint64_t>(data.size()) * 8u;
            for (const std::uint8_t byte : data)
            {
                block[used++] = byte;
                if (used == 64) { compress(); used = 0; }
            }
        }

        auto finish() noexcept -> std::array<std::uint8_t, 20>
        {
            const std::uint64_t bits{ length };
            block[used++] = 0x80u;
            if (used > 56)
            {
                while (used < 64) { block[used++] = 0u; }
                compress();
                used = 0;
            }
            while (used < 56) { block[used++] = 0u; }
            for (int i{ 7 }; i >= 0; --i)
            {
                block[used++] = static_cast<std::uint8_t>((bits >> (i * 8)) & 0xFFu);
            }
            compress();

            std::array<std::uint8_t, 20> out{};
            for (std::size_t i{ 0 }; i < 5; ++i)
            {
                out[i * 4 + 0] = static_cast<std::uint8_t>((state[i] >> 24) & 0xFFu);
                out[i * 4 + 1] = static_cast<std::uint8_t>((state[i] >> 16) & 0xFFu);
                out[i * 4 + 2] = static_cast<std::uint8_t>((state[i] >> 8) & 0xFFu);
                out[i * 4 + 3] = static_cast<std::uint8_t>(state[i] & 0xFFu);
            }
            return out;
        }
    };

    inline auto base64(const std::span<const std::uint8_t> data) -> std::string
    {
        static constexpr std::string_view alphabet{
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/" };
        std::string out;
        out.reserve(((data.size() + 2u) / 3u) * 4u);

        std::size_t i{ 0 };
        for (; i + 2 < data.size(); i += 3)
        {
            const std::uint32_t v{ (static_cast<std::uint32_t>(data[i]) << 16)
                                 | (static_cast<std::uint32_t>(data[i + 1]) << 8)
                                 |  static_cast<std::uint32_t>(data[i + 2]) };
            out += alphabet[(v >> 18) & 0x3Fu];
            out += alphabet[(v >> 12) & 0x3Fu];
            out += alphabet[(v >> 6) & 0x3Fu];
            out += alphabet[v & 0x3Fu];
        }
        if (i < data.size())
        {
            std::uint32_t v{ static_cast<std::uint32_t>(data[i]) << 16 };
            const bool two{ i + 1 < data.size() };
            if (two) { v |= static_cast<std::uint32_t>(data[i + 1]) << 8; }
            out += alphabet[(v >> 18) & 0x3Fu];
            out += alphabet[(v >> 12) & 0x3Fu];
            out += two ? alphabet[(v >> 6) & 0x3Fu] : '=';
            out += '=';
        }
        return out;
    }
}

namespace chatwire::ws
{
    /* @brief RFC 6455 opcodes.  Only the ones a text protocol needs. */
    enum class opcode : std::uint8_t
    {
        continuation = 0x0,
        text         = 0x1,
        binary       = 0x2,
        close        = 0x8,
        ping         = 0x9,
        pong         = 0xA,
    };

    /*
        @brief The Sec-WebSocket-Accept value for a client's Sec-WebSocket-Key.
        @details
        RFC 6455 §4.2.2: concatenate the key with the fixed GUID, SHA-1 it,
        base64 the digest.  The GUID is a constant of the protocol, not a
        secret.
    */
    [[nodiscard]] inline auto accept_key(const std::string_view client_key)
        -> std::string
    {
        static constexpr std::string_view guid{ "258EAFA5-E914-47DA-95CA-C5AB0DC85B11" };
        detail::sha1 hash{};
        hash.update({ reinterpret_cast<const std::uint8_t*>(client_key.data()), client_key.size() });
        hash.update({ reinterpret_cast<const std::uint8_t*>(guid.data()), guid.size() });
        const auto digest{ hash.finish() };
        return detail::base64(digest);
    }

    /*
        @brief Encodes one unfragmented server->client frame.
        @details
        Server frames are never masked (RFC 6455 §5.1), so this writes the
        header and appends the payload verbatim.  The three length forms are
        the protocol's, not a choice: <126 inline, <=0xFFFF as 16 bits, else 64.
    */
    [[nodiscard]] inline auto encode(const opcode op, const std::string_view payload)
        -> std::vector<std::uint8_t>
    {
        std::vector<std::uint8_t> frame;
        frame.reserve(payload.size() + 10u);
        frame.push_back(static_cast<std::uint8_t>(0x80u | static_cast<std::uint8_t>(op)));  // FIN + opcode

        const std::size_t n{ payload.size() };
        if (n < 126u)
        {
            frame.push_back(static_cast<std::uint8_t>(n));
        }
        else if (n <= 0xFFFFu)
        {
            frame.push_back(126u);
            frame.push_back(static_cast<std::uint8_t>((n >> 8) & 0xFFu));
            frame.push_back(static_cast<std::uint8_t>(n & 0xFFu));
        }
        else
        {
            frame.push_back(127u);
            for (int i{ 7 }; i >= 0; --i)
            {
                frame.push_back(static_cast<std::uint8_t>((static_cast<std::uint64_t>(n) >> (i * 8)) & 0xFFu));
            }
        }
        frame.insert(frame.end(), payload.begin(), payload.end());
        return frame;
    }

    /* @brief What decode() managed to do with the bytes it was given. */
    enum class decode_status : std::uint8_t
    {
        /* A whole frame came out; `consumed` bytes belong to it. */
        ok,
        /* Not enough bytes yet.  Keep buffering and call again. */
        incomplete,
        /* The peer violated the protocol.  The caller must close the connection. */
        protocol_error,
    };

    struct frame
    {
        opcode      op{ opcode::text };
        bool        fin{ true };
        std::string payload{};
    };

    /*
        @brief Decodes one client->server frame from the front of `buffer`.
        @details
        Enforces the two rules a server cannot let slide:

          * CLIENT FRAMES MUST BE MASKED (§5.1).  An unmasked client frame is a
            protocol error, and treating it as valid is the classic way a
            hand-written server ends up echoing attacker-chosen bytes.
          * CONTROL FRAMES ARE <=125 BYTES AND NEVER FRAGMENTED (§5.5).  A
            control frame claiming a 64-bit length is malformed.

        A 64-bit length with the high bit set is also rejected: §5.2 reserves it,
        and accepting it invites a size_t overflow on 32-bit builds.

        @param buffer    Bytes received so far.
        @param out       Filled on `ok`.
        @param consumed  Bytes of `buffer` the frame occupied, on `ok`.
    */
    [[nodiscard]] inline auto decode(const std::span<const std::uint8_t> buffer,
                                     frame& out,
                                     std::size_t& consumed) noexcept
        -> decode_status
    {
        if (buffer.size() < 2u) { return decode_status::incomplete; }

        const bool          fin{ (buffer[0] & 0x80u) != 0u };
        const std::uint8_t  reserved{ static_cast<std::uint8_t>(buffer[0] & 0x70u) };
        const auto          op{ static_cast<opcode>(buffer[0] & 0x0Fu) };
        const bool          masked{ (buffer[1] & 0x80u) != 0u };
        std::uint64_t       length{ static_cast<std::uint64_t>(buffer[1] & 0x7Fu) };

        // We negotiate no extensions, so a reserved bit can only mean the peer
        // is speaking something we did not agree to.
        if (reserved != 0u) { return decode_status::protocol_error; }
        if (!masked)        { return decode_status::protocol_error; }

        const bool is_control{ (static_cast<std::uint8_t>(op) & 0x08u) != 0u };
        if (is_control && (length > 125u || !fin)) { return decode_status::protocol_error; }

        std::size_t offset{ 2u };
        if (length == 126u)
        {
            if (buffer.size() < offset + 2u) { return decode_status::incomplete; }
            length = (static_cast<std::uint64_t>(buffer[offset]) << 8)
                   |  static_cast<std::uint64_t>(buffer[offset + 1]);
            offset += 2u;
        }
        else if (length == 127u)
        {
            if (buffer.size() < offset + 8u) { return decode_status::incomplete; }
            length = 0u;
            for (std::size_t i{ 0 }; i < 8u; ++i)
            {
                length = (length << 8) | static_cast<std::uint64_t>(buffer[offset + i]);
            }
            offset += 8u;
            if ((length >> 63) != 0u) { return decode_status::protocol_error; }
        }

        // A frame larger than anything this protocol sends is a resource attack,
        // not a message.  16 MiB is far above any chat line.
        static constexpr std::uint64_t max_payload{ 16ull * 1024ull * 1024ull };
        if (length > max_payload) { return decode_status::protocol_error; }

        if (buffer.size() < offset + 4u) { return decode_status::incomplete; }
        std::array<std::uint8_t, 4> mask{ buffer[offset], buffer[offset + 1],
                                          buffer[offset + 2], buffer[offset + 3] };
        offset += 4u;

        if (buffer.size() < offset + length) { return decode_status::incomplete; }

        out.op  = op;
        out.fin = fin;
        out.payload.resize(static_cast<std::size_t>(length));
        for (std::uint64_t i{ 0 }; i < length; ++i)
        {
            out.payload[static_cast<std::size_t>(i)] =
                static_cast<char>(buffer[offset + static_cast<std::size_t>(i)] ^ mask[i % 4u]);
        }

        consumed = offset + static_cast<std::size_t>(length);
        return decode_status::ok;
    }
}
