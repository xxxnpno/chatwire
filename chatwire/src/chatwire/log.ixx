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

export module chatwire.log;

// chatwire.core.log — diagnostics that cannot take the host process down.
//
// This library runs inside someone else's Minecraft.  A logger that throws, or
// that touches a destroyed global during DLL unload, kills their game.  So:
//
//   * every entry point is noexcept and swallows its own failures;
//   * the sink is a function-local static, constructed on first use and never
//     destroyed, which removes static-destruction-order from the picture
//     entirely (a logger that outlives its mutex is a crash at exit);
//   * writes are serialised, because a torn line from two threads is worse than
//     no line.

export namespace chatwire::log
{
    /*
        @brief How much noise reaches the console.
        @details
        Defaults to `warning`, deliberately.  The console this writes to sits in
        front of a person watching their game's chat scroll past; a start-up
        trace they cannot act on is noise competing with the thing they actually
        opened it for.  Anything that needs attention -- a hook that would not
        install, a mapping that resolved to nothing -- is a warning or an error
        and still shows.

        Set CHATWIRE_VERBOSE=1 in the game's environment to get the trace back.
    */
    enum class level : std::uint8_t
    {
        info    = 0,
        warning = 1,
        error   = 2,
        silent  = 3,
    };

    namespace detail
    {
        inline std::atomic<level> g_threshold{ level::warning };
    }

    /* @brief Nothing below `minimum` is printed. */
    inline auto set_level(const level minimum) noexcept -> void
    {
        detail::g_threshold.store(minimum, std::memory_order_release);
    }

    [[nodiscard]] inline auto current_level() noexcept -> level
    {
        return detail::g_threshold.load(std::memory_order_acquire);
    }
}

export namespace chatwire::log::detail
{
    // Deliberately leaked: `new` with no delete.  A destroyed mutex that a
    // detour still logs through is a use-after-free in a foreign process; a
    // leaked one at exit is nothing, because the process is going away.
    inline auto sink_mutex() noexcept -> std::mutex&
    {
        static std::mutex* const m{ new std::mutex{} };
        return *m;
    }

    inline auto enabled(const chatwire::log::level at) noexcept -> bool
    {
        return static_cast<std::uint8_t>(at)
               >= static_cast<std::uint8_t>(g_threshold.load(std::memory_order_acquire));
    }

    inline auto write_raw(const std::string_view line) noexcept -> void
    {
        try
        {
            const std::lock_guard<std::mutex> guard{ sink_mutex() };
            std::fprintf(stdout, "%.*s\n", static_cast<int>(line.size()), line.data());
            std::fflush(stdout);
        }
        catch (...) { }
    }

    inline auto write(const std::string_view level, const std::string_view text) noexcept -> void
    {
        try
        {
            const std::lock_guard<std::mutex> guard{ sink_mutex() };
            std::fprintf(stdout, "[chatwire] [%.*s] %.*s\n",
                         static_cast<int>(level.size()), level.data(),
                         static_cast<int>(text.size()), text.data());
            std::fflush(stdout);
        }
        catch (...)
        {
            // A logger that propagates is a logger that crashes the game.
        }
    }
}

export namespace chatwire::log
{
    /*
        @brief Formatted logging that never throws and never escapes.
        @details
        std::format can throw (bad_alloc, format_error).  Catching here means a
        malformed log call degrades to a fixed marker instead of unwinding out
        of a detour into the JVM's interpreter, where there is no handler and
        the result is a hard crash.
    */
    /*
        @brief Writes a line verbatim, bypassing the level filter and the prefix.
        @details
        For output that IS the point rather than a note about it: the console
        banner, and the chat lines the user opened the window to read.  Filtering
        those would be filtering the product.
    */
    inline auto raw(const std::string_view line) noexcept -> void
    {
        detail::write_raw(line);
    }

    template<typename... args_t>
    inline auto info(const std::format_string<args_t...> fmt, args_t&&... args) noexcept -> void
    {
        if (!detail::enabled(level::info)) { return; }
        try { detail::write("info", std::format(fmt, std::forward<args_t>(args)...)); }
        catch (...) { detail::write("info", "<message could not be formatted>"); }
    }

    template<typename... args_t>
    inline auto warn(const std::format_string<args_t...> fmt, args_t&&... args) noexcept -> void
    {
        if (!detail::enabled(level::warning)) { return; }
        try { detail::write("warn", std::format(fmt, std::forward<args_t>(args)...)); }
        catch (...) { detail::write("warn", "<message could not be formatted>"); }
    }

    template<typename... args_t>
    inline auto error(const std::format_string<args_t...> fmt, args_t&&... args) noexcept -> void
    {
        if (!detail::enabled(level::error)) { return; }
        try { detail::write("error", std::format(fmt, std::forward<args_t>(args)...)); }
        catch (...) { detail::write("error", "<message could not be formatted>"); }
    }
}
