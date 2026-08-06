#pragma once

// chatwire.net — Winsock, behind a small vocabulary.
//
// Not a portability layer: chatwire is Windows-only.  This exists so that the
// server is written against `send_some` / `wait_readable` / `retryable` rather
// than against Winsock's spellings, and so the three things that are easy to
// get wrong are decided once, here, rather than at each call site.
//
//   SOCKET NUMBERS.  A Winsock handle is an unsigned pointer-sized type and the
//   invalid one is ~0, not -1 and not 0.  Comparing a socket against 0 is
//   therefore always wrong, which is why `invalid_socket` exists and no call
//   site writes a literal.
//
//   RETRYABLE ERRORS.  WSAEINTR is the one that bites: a JVM sends plenty of
//   its own signals, for safepoints and for the sampling profiler, and treating
//   an interrupted call as a failure would drop a client every time the
//   collector ran.
//
//   WAKING A BLOCKED ACCEPT.  Closing a listening socket from another thread
//   does wake accept() on Windows -- but chatwire polls with wait_readable()
//   instead, so the accept thread owns its own exit and nothing ever closes a
//   descriptor another thread is blocked on.  See ws/server.hpp.
#include "chatwire/common.hpp"

// Platform headers strictly after the standard ones -- see common.hpp.
#include <winsock2.h>
#include <ws2tcpip.h>

namespace chatwire::net
{
    using socket_t = ::SOCKET;
    using addr_len = int;

    inline constexpr socket_t invalid_socket{ INVALID_SOCKET };

    [[nodiscard]] inline auto to_network_port(const std::uint16_t port) noexcept
        -> std::uint16_t
    {
        return ::htons(port);
    }

    [[nodiscard]] inline auto from_network_port(const std::uint16_t port) noexcept
        -> std::uint16_t
    {
        return ::ntohs(port);
    }

    /* @brief 127.0.0.1, in network byte order.  The only address chatwire binds. */
    [[nodiscard]] inline auto loopback_address() noexcept -> std::uint32_t
    {
        return ::htonl(INADDR_LOOPBACK);
    }

    /* @brief Brings Winsock up.  It refuses every call until it has had this. */
    [[nodiscard]] inline auto startup() noexcept -> bool
    {
        ::WSADATA wsa{};
        return ::WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }

    inline auto cleanup() noexcept -> void
    {
        (void)::WSACleanup();
    }

    /* @brief The last socket error on the calling thread. */
    [[nodiscard]] inline auto last_error() noexcept -> int
    {
        return ::WSAGetLastError();
    }

    /* @brief True when `error` means "nothing went wrong, try again". */
    [[nodiscard]] inline auto retryable(const int error) noexcept -> bool
    {
        return error == WSAEINTR || error == WSAEWOULDBLOCK;
    }

    inline auto close_socket(const socket_t sock) noexcept -> void
    {
        if (sock == invalid_socket) { return; }
        (void)::closesocket(sock);
    }

    /*
        @brief Half-closes both directions, waking a thread blocked in recv().
        @details
        This is how a client reader is woken, rather than by closing its socket:
        the socket stays valid and its owner still closes it, so no other thread
        is holding a descriptor that has already gone back to the OS.
    */
    inline auto shutdown_both(const socket_t sock) noexcept -> void
    {
        if (sock == invalid_socket) { return; }
        (void)::shutdown(sock, SD_BOTH);
    }

    /* @brief Per-socket setup before any traffic.  Nothing to do on Winsock. */
    inline auto prepare(const socket_t sock) noexcept -> void
    {
        (void)sock;
    }

    /*
        @brief Lets a restart rebind a port still in TIME_WAIT.
        @details
        Without it, stopping and restarting chatwire inside one game session
        fails to bind for about two minutes and looks like a bug in chatwire.
    */
    inline auto set_reuse_addr(const socket_t sock) noexcept -> void
    {
        const int on{ 1 };
        (void)::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
                           reinterpret_cast<const char*>(&on), sizeof(on));
    }

    /*
        @brief Sends what it can, once.
        @return bytes written, or <= 0 on error; check retryable(last_error()).
    */
    [[nodiscard]] inline auto send_some(const socket_t sock, const void* const data,
                                        const std::size_t size) noexcept -> std::ptrdiff_t
    {
        return ::send(sock, static_cast<const char*>(data), static_cast<int>(size), 0);
    }

    [[nodiscard]] inline auto recv_some(const socket_t sock, void* const data,
                                        const std::size_t size) noexcept -> std::ptrdiff_t
    {
        return ::recv(sock, static_cast<char*>(data), static_cast<int>(size), 0);
    }

    /*
        @brief Waits up to `timeout_ms` for `sock` to have something to read.
        @details
        A listening socket becomes readable exactly when a connection is
        pending, so the accept loop polls this, notices the shutdown flag
        between polls, and leaves on its own -- without any other thread
        touching a descriptor it is blocked on.

        @return >0 readable, 0 timed out, <0 error.
    */
    [[nodiscard]] inline auto wait_readable(const socket_t sock,
                                            const int timeout_ms) noexcept -> int
    {
        if (sock == invalid_socket) { return -1; }
        ::fd_set read_set{};
        FD_ZERO(&read_set);
        FD_SET(sock, &read_set);
        ::timeval tv{ .tv_sec  = timeout_ms / 1000,
                      .tv_usec = (timeout_ms % 1000) * 1000 };
        // Winsock ignores the first argument; it is there for BSD source
        // compatibility and nothing else.
        return ::select(0, &read_set, nullptr, nullptr, &tv);
    }
}
