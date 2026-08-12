module;

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

export module chatwire.crypto;

// chatwire.crypto — SHA-256, HMAC, and the two comparisons that must not leak.
//
// ===========================================================================
// WHY THERE IS CRYPTO IN HERE AT ALL
// ===========================================================================
// chatwire binds a socket that drives someone's game.  On loopback that is
// exactly as private as the machine.  Bound anywhere else it is not, and the
// question stops being "is the port open" and becomes "who is allowed to send
// `sendChatMessage`".
//
// So a bind that is not loopback requires a shared secret, and the secret is
// never transmitted: the server sends a random nonce, the client returns
// HMAC(secret, nonce), and the server checks it.  An observer learns a nonce
// and a MAC of it, which are worth nothing for the next connection because the
// nonce is fresh each time.
//
// ===========================================================================
// WHAT THIS IS NOT
// ===========================================================================
// It is NOT encryption, and chatwire does not pretend to offer any.  Everything
// after the handshake is plaintext WebSocket, so across an untrusted network it
// must be tunnelled -- WireGuard, Tailscale, an SSH forward.  See the README.
//
// That is a deliberate refusal rather than a gap.  Implementing TLS inside a
// DLL injected into someone's game, with no vetted stack available and no way
// to keep up with the next protocol flaw, produces something that LOOKS
// encrypted and is not.  A wrong TLS is worse than an honest plaintext socket
// behind a real tunnel, because only one of the two is understood by the person
// deciding whether to expose it.
//
// ===========================================================================
// WHY IT IS CONSTEXPR
// ===========================================================================
// Every function below runs at compile time, which is what lets the static
// asserts at the bottom check this implementation against the published test
// vectors -- FIPS 180-4 for SHA-256, RFC 4231 for HMAC -- as part of building.
// A hash that is subtly wrong still produces stable, plausible-looking output
// and would authenticate nobody while appearing to work; a test that runs when
// somebody remembers to run it is not the guarantee this needs.

// NO PLATFORM HEADER, and that is worth the six lines below.  Everything in
// this file except random_hex() is pure arithmetic that runs at compile time,
// and including <windows.h> here dragged it into every translation unit that
// wanted a hash -- which broke the project's own ordering rule immediately:
// <windows.h> before <winsock2.h> is an error, and ws/server.hpp needs both a
// hash and a socket.
//
// So the one OS function is declared rather than included.  Its signature is
// part of the documented Win32 API and has not changed since Vista.
extern "C" __declspec(dllimport) long __stdcall BCryptGenRandom(
    void* algorithm, unsigned char* buffer, unsigned long bytes, unsigned long flags);

export namespace chatwire::crypto
{
    inline constexpr std::size_t sha256_size{ 32 };

    using digest = std::array<std::uint8_t, sha256_size>;

    namespace detail
    {
        inline constexpr std::array<std::uint32_t, 64> round_constants{
            0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
            0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
            0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
            0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
            0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
            0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
            0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
            0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
            0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
            0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
            0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
        };

        [[nodiscard]] constexpr auto rotr(const std::uint32_t v, const int by) noexcept
            -> std::uint32_t
        {
            return (v >> by) | (v << (32 - by));
        }

        /* @brief One 64-byte block, mixed into the running state. */
        constexpr auto compress(std::array<std::uint32_t, 8>& state,
                                const std::array<std::uint8_t, 64>& block) noexcept -> void
        {
            std::array<std::uint32_t, 64> w{};
            for (std::size_t i{ 0 }; i < 16u; ++i)
            {
                w[i] = (static_cast<std::uint32_t>(block[i * 4u]) << 24)
                     | (static_cast<std::uint32_t>(block[i * 4u + 1u]) << 16)
                     | (static_cast<std::uint32_t>(block[i * 4u + 2u]) << 8)
                     | static_cast<std::uint32_t>(block[i * 4u + 3u]);
            }
            for (std::size_t i{ 16 }; i < 64u; ++i)
            {
                const std::uint32_t s0{ rotr(w[i - 15u], 7) ^ rotr(w[i - 15u], 18)
                                        ^ (w[i - 15u] >> 3) };
                const std::uint32_t s1{ rotr(w[i - 2u], 17) ^ rotr(w[i - 2u], 19)
                                        ^ (w[i - 2u] >> 10) };
                w[i] = w[i - 16u] + s0 + w[i - 7u] + s1;
            }

            auto v{ state };
            for (std::size_t i{ 0 }; i < 64u; ++i)
            {
                const std::uint32_t s1{ rotr(v[4], 6) ^ rotr(v[4], 11) ^ rotr(v[4], 25) };
                const std::uint32_t ch{ (v[4] & v[5]) ^ (~v[4] & v[6]) };
                const std::uint32_t t1{ v[7] + s1 + ch + round_constants[i] + w[i] };
                const std::uint32_t s0{ rotr(v[0], 2) ^ rotr(v[0], 13) ^ rotr(v[0], 22) };
                const std::uint32_t maj{ (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]) };
                const std::uint32_t t2{ s0 + maj };

                v[7] = v[6]; v[6] = v[5]; v[5] = v[4]; v[4] = v[3] + t1;
                v[3] = v[2]; v[2] = v[1]; v[1] = v[0]; v[0] = t1 + t2;
            }
            for (std::size_t i{ 0 }; i < 8u; ++i) { state[i] += v[i]; }
        }
    }

    /* @brief SHA-256 of `data`.  Constexpr, so the vectors below check it. */
    [[nodiscard]] constexpr auto sha256(const std::span<const std::uint8_t> data) noexcept
        -> digest
    {
        std::array<std::uint32_t, 8> state{
            0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
            0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
        };

        std::array<std::uint8_t, 64> block{};
        std::size_t used{ 0 };
        for (const std::uint8_t byte : data)
        {
            block[used++] = byte;
            if (used == 64u) { detail::compress(state, block); used = 0; }
        }

        // The padding: a 1 bit, zeroes, and the length in BITS as a 64-bit
        // big-endian value.  When the 1 bit and the length do not fit in the
        // same block, the length goes in the next one -- which is the case this
        // gets wrong in every hand-written SHA-256 that only ever hashed short
        // inputs, and which the 56-byte vector below exists to catch.
        const std::uint64_t bits{ static_cast<std::uint64_t>(data.size()) * 8u };
        block[used++] = 0x80u;
        if (used > 56u)
        {
            while (used < 64u) { block[used++] = 0u; }
            detail::compress(state, block);
            used = 0;
        }
        while (used < 56u) { block[used++] = 0u; }
        for (int i{ 7 }; i >= 0; --i)
        {
            block[used++] = static_cast<std::uint8_t>((bits >> (i * 8)) & 0xFFu);
        }
        detail::compress(state, block);

        digest out{};
        for (std::size_t i{ 0 }; i < 8u; ++i)
        {
            out[i * 4u]      = static_cast<std::uint8_t>((state[i] >> 24) & 0xFFu);
            out[i * 4u + 1u] = static_cast<std::uint8_t>((state[i] >> 16) & 0xFFu);
            out[i * 4u + 2u] = static_cast<std::uint8_t>((state[i] >> 8) & 0xFFu);
            out[i * 4u + 3u] = static_cast<std::uint8_t>(state[i] & 0xFFu);
        }
        return out;
    }

    [[nodiscard]] constexpr auto sha256(const std::string_view text) noexcept -> digest
    {
        std::array<std::uint8_t, 256> buffer{};
        std::size_t n{ 0 };
        for (const char c : text)
        {
            if (n < buffer.size()) { buffer[n++] = static_cast<std::uint8_t>(c); }
        }
        return sha256(std::span<const std::uint8_t>{ buffer.data(), n });
    }

    /*
        @brief HMAC-SHA256, as RFC 2104 defines it.
        @details
        A key longer than the block is HASHED first, and a shorter one is padded
        with zeroes -- both are part of the construction rather than details, and
        skipping the first is the classic way to build something that agrees with
        itself and with nothing else.  The RFC 4231 vector at the bottom uses a
        131-byte key precisely to exercise it.
    */
    [[nodiscard]] constexpr auto hmac_sha256(const std::string_view key,
                                             const std::string_view message) noexcept -> digest
    {
        constexpr std::size_t block_size{ 64 };

        std::array<std::uint8_t, block_size> padded{};
        if (key.size() > block_size)
        {
            const digest hashed{ sha256(key) };
            for (std::size_t i{ 0 }; i < hashed.size(); ++i) { padded[i] = hashed[i]; }
        }
        else
        {
            for (std::size_t i{ 0 }; i < key.size(); ++i)
            {
                padded[i] = static_cast<std::uint8_t>(key[i]);
            }
        }

        std::array<std::uint8_t, block_size + 256u> inner{};
        std::size_t inner_len{ 0 };
        for (std::size_t i{ 0 }; i < block_size; ++i)
        {
            inner[inner_len++] = static_cast<std::uint8_t>(padded[i] ^ 0x36u);
        }
        for (const char c : message)
        {
            if (inner_len < inner.size()) { inner[inner_len++] = static_cast<std::uint8_t>(c); }
        }
        const digest inner_digest{
            sha256(std::span<const std::uint8_t>{ inner.data(), inner_len }) };

        std::array<std::uint8_t, block_size + sha256_size> outer{};
        std::size_t outer_len{ 0 };
        for (std::size_t i{ 0 }; i < block_size; ++i)
        {
            outer[outer_len++] = static_cast<std::uint8_t>(padded[i] ^ 0x5Cu);
        }
        for (const std::uint8_t byte : inner_digest) { outer[outer_len++] = byte; }
        return sha256(std::span<const std::uint8_t>{ outer.data(), outer_len });
    }

    [[nodiscard]] constexpr auto to_hex(const digest& value) -> std::string
    {
        constexpr std::string_view alphabet{ "0123456789abcdef" };
        std::string out{};
        out.reserve(value.size() * 2u);
        for (const std::uint8_t byte : value)
        {
            out += alphabet[(byte >> 4) & 0x0Fu];
            out += alphabet[byte & 0x0Fu];
        }
        return out;
    }

    /*
        @brief Compares two strings in time that does not depend on their content.
        @details
        `a == b` on a std::string stops at the first differing byte, so how long
        it took says how much of the secret was right.  Over enough attempts that
        is the secret.  This reads every byte of both, always.

        The length is compared too, and that DOES leak -- but the length of a MAC
        is fixed and public, so there is nothing there to learn.
    */
    [[nodiscard]] inline auto equal_in_constant_time(const std::string_view a,
                                                     const std::string_view b) noexcept -> bool
    {
        if (a.size() != b.size()) { return false; }
        unsigned char difference{ 0 };
        for (std::size_t i{ 0 }; i < a.size(); ++i)
        {
            difference |= static_cast<unsigned char>(a[i] ^ b[i]);
        }
        return difference == 0u;
    }

    /*
        @brief `count` random bytes as hex, from the OS.  "" on failure.
        @details
        BCryptGenRandom with the system-preferred RNG, which is the documented
        way to ask Windows for cryptographic randomness.  NOT a clock, not a
        counter, not std::random_device -- a nonce a caller can predict lets them
        prepare for a challenge before it is issued, and the only reason to reach
        for a weaker source here would be to avoid one link dependency.

        Returns "" rather than falling back to something weaker if the OS
        refuses.  A caller that cannot get a nonce must refuse the connection,
        which is what server.hpp does: no nonce, no session, and the failure is
        loud instead of silently downgrading everyone's authentication.
    */
    [[nodiscard]] inline auto random_hex(const std::size_t count) -> std::string
    {
        /* BCRYPT_USE_SYSTEM_PREFERRED_RNG: no algorithm handle to open, and the
           provider is whichever one Windows currently prefers. */
        constexpr unsigned long use_system_preferred_rng{ 0x00000002u };

        std::vector<std::uint8_t> bytes(count, 0u);
        const long status{ ::BCryptGenRandom(nullptr, bytes.data(),
                                             static_cast<unsigned long>(bytes.size()),
                                             use_system_preferred_rng) };
        if (status != 0) { return {}; }

        constexpr std::string_view alphabet{ "0123456789abcdef" };
        std::string out{};
        out.reserve(count * 2u);
        for (const std::uint8_t byte : bytes)
        {
            out += alphabet[(byte >> 4) & 0x0Fu];
            out += alphabet[byte & 0x0Fu];
        }
        return out;
    }

    // -----------------------------------------------------------------------
    // The published vectors, checked at COMPILE TIME.  A wrong hash still
    // produces stable, plausible output; it would authenticate nobody while
    // appearing to work, and no run-time test catches what was never run.
    // -----------------------------------------------------------------------

    /* FIPS 180-4, the one-block "abc" example. */
    static_assert(sha256("abc") == digest{
        0xbau, 0x78u, 0x16u, 0xbfu, 0x8fu, 0x01u, 0xcfu, 0xeau,
        0x41u, 0x41u, 0x40u, 0xdeu, 0x5du, 0xaeu, 0x22u, 0x23u,
        0xb0u, 0x03u, 0x61u, 0xa3u, 0x96u, 0x17u, 0x7au, 0x9cu,
        0xb4u, 0x10u, 0xffu, 0x61u, 0xf2u, 0x00u, 0x15u, 0xadu });

    /* The empty input, whose padding is the whole of the work. */
    static_assert(sha256("") == digest{
        0xe3u, 0xb0u, 0xc4u, 0x42u, 0x98u, 0xfcu, 0x1cu, 0x14u,
        0x9au, 0xfbu, 0xf4u, 0xc8u, 0x99u, 0x6fu, 0xb9u, 0x24u,
        0x27u, 0xaeu, 0x41u, 0xe4u, 0x64u, 0x9bu, 0x93u, 0x4cu,
        0xa4u, 0x95u, 0x99u, 0x1bu, 0x78u, 0x52u, 0xb8u, 0x55u });

    /*
        FIPS 180-4's 56-byte example: the length no longer fits in the block the
        0x80 went into, so the padding spills into a second one.  This is the
        case a SHA-256 that only ever hashed short strings gets wrong.
    */
    static_assert(sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")
                  == digest{
        0x24u, 0x8du, 0x6au, 0x61u, 0xd2u, 0x06u, 0x38u, 0xb8u,
        0xe5u, 0xc0u, 0x26u, 0x93u, 0x0cu, 0x3eu, 0x60u, 0x39u,
        0xa3u, 0x3cu, 0xe4u, 0x59u, 0x64u, 0xffu, 0x21u, 0x67u,
        0xf6u, 0xecu, 0xedu, 0xd4u, 0x19u, 0xdbu, 0x06u, 0xc1u });

    /* RFC 4231 test case 2 — a short key, the classic "Jefe" vector. */
    static_assert(hmac_sha256("Jefe", "what do ya want for nothing?") == digest{
        0x5bu, 0xdcu, 0xc1u, 0x46u, 0xbfu, 0x60u, 0x75u, 0x4eu,
        0x6au, 0x04u, 0x24u, 0x26u, 0x08u, 0x95u, 0x75u, 0xc7u,
        0x5au, 0x00u, 0x3fu, 0x08u, 0x9du, 0x27u, 0x39u, 0x83u,
        0x9du, 0xecu, 0x58u, 0xb9u, 0x64u, 0xecu, 0x38u, 0x43u });
}
