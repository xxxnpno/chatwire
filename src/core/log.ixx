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
module;

// The shared preamble, FIRST and identical in every module.  See the header
// for why GCC 15 requires that of a modular build.
#include "../core/prelude.hpp"

export module chatwire.core.log;

namespace chatwire::log::detail
{
    // Deliberately leaked: `new` with no delete.  A destroyed mutex that a
    // detour still logs through is a use-after-free in a foreign process; a
    // leaked one at exit is nothing, because the process is going away.
    inline auto sink_mutex() noexcept -> std::mutex&
    {
        static std::mutex* const m{ new std::mutex{} };
        return *m;
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
    template<typename... args_t>
    inline auto info(const std::format_string<args_t...> fmt, args_t&&... args) noexcept -> void
    {
        try { detail::write("info", std::format(fmt, std::forward<args_t>(args)...)); }
        catch (...) { detail::write("info", "<message could not be formatted>"); }
    }

    template<typename... args_t>
    inline auto warn(const std::format_string<args_t...> fmt, args_t&&... args) noexcept -> void
    {
        try { detail::write("warn", std::format(fmt, std::forward<args_t>(args)...)); }
        catch (...) { detail::write("warn", "<message could not be formatted>"); }
    }

    template<typename... args_t>
    inline auto error(const std::format_string<args_t...> fmt, args_t&&... args) noexcept -> void
    {
        try { detail::write("error", std::format(fmt, std::forward<args_t>(args)...)); }
        catch (...) { detail::write("error", "<message could not be formatted>"); }
    }
}
