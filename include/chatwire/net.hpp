#pragma once

// chatwire.net — the one header that knows sockets differ between platforms.
//
// ===========================================================================
// WHY THIS EXISTS
// ===========================================================================
// Winsock and BSD sockets are close enough to look interchangeable and far
// enough apart to break in ways that only show up at runtime, on the platform
// you were not testing on.  Rather than sprinkle `#if defined(_WIN32)` through
// the server, every difference is resolved exactly once, here, and the rest of
// chatwire is written against a single vocabulary.
//
// The differences that actually matter, and what each one would have cost:
//
//   SIGPIPE.  Writing to a socket whose peer has gone raises SIGPIPE on POSIX,
//   whose default disposition is to KILL THE PROCESS.  chatwire runs inside
//   somebody's Minecraft, so the cost of getting this wrong is not a dropped
//   message, it is the game closing because a client pressed Ctrl+C.  Suppressed
//   per-call with MSG_NOSIGNAL where it exists (Linux) and per-socket with
//   SO_NOSIGPIPE where it does not (macOS) -- never with signal(SIGPIPE, SIG_IGN),
//   because process-wide signal state belongs to the host program and an
//   injected library has no business changing it.
//
//   Return and length types.  POSIX send/recv take size_t and return ssize_t;
//   Winsock takes and returns int.  `const int n{ ::recv(...) }` is a narrowing
//   conversion in list-initialisation on POSIX -- a hard error, not a warning,
//   which is the good case.  Both are normalised to ptrdiff_t here.
//
//   Waking a blocked accept().  On Windows, closing the listening socket from
//   another thread wakes the thread inside accept().  On Linux and macOS it does
//   not reliably do anything at all: the fd number is freed while a thread is
//   still blocked on it, which is both a hang and a use-after-free waiting for
//   that number to be reused.  So nothing in chatwire closes a socket another
//   thread is blocked on any more; wait_readable() below is polled instead, and
//   the shutdown ordering was rewritten around it.  See ws/server.hpp.
//
//   Error reporting.  errno versus WSAGetLastError, with disjoint numbering.
//
// SOCKET NUMBERS.  POSIX file descriptors are small signed ints and the invalid
// one is -1; Winsock handles are an unsigned pointer-sized type and the invalid
// one is ~0.  Comparing a socket against 0 is therefore wrong on exactly one of
// the two platforms, which is why `invalid_socket` exists and no call site
// writes a literal.
#include "chatwire/common.hpp"

// Platform headers strictly after the standard ones -- see common.hpp.
#if defined(_WIN32)
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <arpa/inet.h>
    #include <cerrno>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <poll.h>
    #include <sys/socket.h>
    #include <sys/types.h>
    #include <unistd.h>
#endif

namespace chatwire::net
{
#if defined(_WIN32)
    using socket_t = ::SOCKET;
    using addr_len = int;

    inline constexpr socket_t invalid_socket{ INVALID_SOCKET };
#else
    using socket_t = int;
    using addr_len = ::socklen_t;

    inline constexpr socket_t invalid_socket{ -1 };
#endif

    /*
        @brief Brings the socket library up.  Idempotent per process on POSIX.
        @details
        Winsock genuinely needs this and refuses every call until it has had it.
        POSIX has nothing to initialise, so this is `true` and costs nothing --
        which is the point of it existing at all: the call site does not get to
        know which platform it is on.
    */
    [[nodiscard]] inline auto startup() noexcept -> bool
    {
#if defined(_WIN32)
        ::WSADATA wsa{};
        return ::WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
        return true;
#endif
    }

    inline auto cleanup() noexcept -> void
    {
#if defined(_WIN32)
        (void)::WSACleanup();
#endif
    }

    /* @brief The last socket error on the calling thread. */
    [[nodiscard]] inline auto last_error() noexcept -> int
    {
#if defined(_WIN32)
        return ::WSAGetLastError();
#else
        return errno;
#endif
    }

    /*
        @brief True when `error` means "nothing went wrong, try again".
        @details
        EINTR is the one that bites: any signal delivered to the process -- and a
        JVM sends plenty of its own, for safepoints and for the sampling profiler
        -- interrupts a blocking socket call and it comes back as an error that
        is not one.  Treating it as a failure would drop a client every time the
        garbage collector ran.
    */
    [[nodiscard]] inline auto retryable(const int error) noexcept -> bool
    {
#if defined(_WIN32)
        return error == WSAEINTR || error == WSAEWOULDBLOCK;
#else
        return error == EINTR || error == EAGAIN || error == EWOULDBLOCK;
#endif
    }

    inline auto close_socket(const socket_t sock) noexcept -> void
    {
        if (sock == invalid_socket) { return; }
#if defined(_WIN32)
        (void)::closesocket(sock);
#else
        (void)::close(sock);
#endif
    }

    /*
        @brief Half-closes both directions, waking a thread blocked in recv().
        @details
        This one IS portable, and it is why the client-reader shutdown path did
        not have to change: shutdown() on a connected socket makes a blocked
        recv() return 0 on Windows, Linux and macOS alike.  The socket stays
        valid and its owner still closes it, so no other thread can be holding a
        descriptor number that has already been handed back to the OS.
    */
    inline auto shutdown_both(const socket_t sock) noexcept -> void
    {
        if (sock == invalid_socket) { return; }
#if defined(_WIN32)
        (void)::shutdown(sock, SD_BOTH);
#else
        (void)::shutdown(sock, SHUT_RDWR);
#endif
    }

    /*
        @brief Per-socket setup that must happen before any traffic.
        @details
        On macOS this is the SIGPIPE guard, and it has to be a socket option
        because Apple's send() has no MSG_NOSIGNAL to pass.  Elsewhere it is a
        no-op and the flag is passed per call instead -- see send_some().
    */
    inline auto prepare(const socket_t sock) noexcept -> void
    {
#if defined(SO_NOSIGPIPE)
        const int on{ 1 };
        (void)::setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE,
                           reinterpret_cast<const char*>(&on), sizeof(on));
#else
        (void)sock;
#endif
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
        @brief Sends what it can, once.  Never raises a signal.
        @return bytes written, or <= 0 on error; check retryable(last_error()).
    */
    [[nodiscard]] inline auto send_some(const socket_t sock, const void* const data,
                                        const std::size_t size) noexcept -> std::ptrdiff_t
    {
#if defined(_WIN32)
        return ::send(sock, static_cast<const char*>(data),
                      static_cast<int>(size), 0);
#elif defined(MSG_NOSIGNAL)
        return ::send(sock, data, size, MSG_NOSIGNAL);
#else
        // Apple: no MSG_NOSIGNAL, so prepare() set SO_NOSIGPIPE on this socket.
        return ::send(sock, data, size, 0);
#endif
    }

    [[nodiscard]] inline auto recv_some(const socket_t sock, void* const data,
                                        const std::size_t size) noexcept -> std::ptrdiff_t
    {
#if defined(_WIN32)
        return ::recv(sock, static_cast<char*>(data), static_cast<int>(size), 0);
#else
        return ::recv(sock, data, size, 0);
#endif
    }

    /*
        @brief Waits up to `timeout_ms` for `sock` to have something to read.
        @details
        The portable replacement for "close the listener and let accept() fail",
        which only works on Windows.  A listening socket becomes readable exactly
        when a connection is pending, so an accept loop can poll this, notice a
        shutdown flag between polls, and leave on its own -- without any other
        thread touching a descriptor it is blocked on.

        poll() rather than select() off Windows: select's fd_set is a bitmap
        capped at FD_SETSIZE (1024 on glibc), and a descriptor above that limit
        is undefined behaviour rather than an error.  chatwire is injected into a
        JVM that has already opened whatever it opened, so its sockets can land
        anywhere -- this is not a hypothetical.

        @return >0 readable, 0 timed out, <0 error.
    */
    [[nodiscard]] inline auto wait_readable(const socket_t sock,
                                            const int timeout_ms) noexcept -> int
    {
        if (sock == invalid_socket) { return -1; }
#if defined(_WIN32)
        ::fd_set read_set{};
        FD_ZERO(&read_set);
        FD_SET(sock, &read_set);
        ::timeval tv{ .tv_sec  = timeout_ms / 1000,
                      .tv_usec = (timeout_ms % 1000) * 1000 };
        // Winsock ignores the first argument; it is there for BSD source
        // compatibility and nothing else.
        return ::select(0, &read_set, nullptr, nullptr, &tv);
#else
        ::pollfd entry{ .fd = sock, .events = POLLIN, .revents = 0 };
        return ::poll(&entry, 1u, timeout_ms);
#endif
    }
}
