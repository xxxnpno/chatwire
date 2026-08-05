#pragma once

// chatwire.terminal — the two things a command-line tool must ask the OS.
//
// Both are Windows-only problems that POSIX solved by default, which is exactly
// why they belong behind a call rather than behind an #if at each use:
//
//   ANSI and UTF-8.  A Unix terminal has understood escape sequences and UTF-8
//   for decades.  A Windows console understands neither until asked: without
//   ENABLE_VIRTUAL_TERMINAL_PROCESSING the colour codes print as literal
//   garbage, and without CP_UTF8 every section sign and every non-ASCII player
//   name arrives as mojibake.  Minecraft chat is full of both.
//
//   Am I attached to a terminal?  Used to decide whether to pause before
//   exiting: a double-clicked console window closes the instant main() returns,
//   taking the error message with it, while a tool in a pipeline must not stop
//   and wait for a keypress that will never come.
//
// This is for the standalone tools.  chatwire's own in-game console has a
// harder job -- it may have to ALLOCATE a console inside a process that has
// none -- and lives in console.hpp.
#include "chatwire/common.hpp"

#if defined(_WIN32)
    #include <windows.h>
#else
    #include <unistd.h>
#endif

namespace chatwire::terminal
{
    /*
        @brief Makes this process's stdout able to show colour and UTF-8.
        @details
        Best-effort by design: a redirected stdout has no console mode to set and
        the call fails, which is correct -- the escape codes then land in the
        file, where whoever reads it can decide what to do with them.
    */
    inline auto enable_ansi() noexcept -> void
    {
#if defined(_WIN32)
        if (const ::HANDLE out{ ::GetStdHandle(STD_OUTPUT_HANDLE) };
            out != INVALID_HANDLE_VALUE)
        {
            ::DWORD mode{ 0 };
            if (::GetConsoleMode(out, &mode))
            {
                (void)::SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
            }
        }
        (void)::SetConsoleOutputCP(CP_UTF8);
#endif
    }

    /* @brief True when stdin is a terminal a human could press enter at. */
    [[nodiscard]] inline auto stdin_is_interactive() noexcept -> bool
    {
#if defined(_WIN32)
        const ::HANDLE in{ ::GetStdHandle(STD_INPUT_HANDLE) };
        ::DWORD        mode{ 0 };
        return in != INVALID_HANDLE_VALUE && ::GetConsoleMode(in, &mode) != 0;
#else
        return ::isatty(STDIN_FILENO) == 1;
#endif
    }
}
