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

#include <windows.h>

export module chatwire.terminal;

// chatwire.terminal — the two things a command-line tool must ask the OS.
//
// Both are things a Windows console will not do until asked:
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

export namespace chatwire::terminal
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
    }

    /* @brief True when stdin is a terminal a human could press enter at. */
    [[nodiscard]] inline auto stdin_is_interactive() noexcept -> bool
    {
        const ::HANDLE in{ ::GetStdHandle(STD_INPUT_HANDLE) };
        ::DWORD        mode{ 0 };
        return in != INVALID_HANDLE_VALUE && ::GetConsoleMode(in, &mode) != 0;
    }
}
