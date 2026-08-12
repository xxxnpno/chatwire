module;

// <cstdio>, and this is the one standard header still included anywhere in
// chatwire's own code.  It is here for THREE MACROS and nothing else.
//
// `stdout`, `stderr` and `stdin` are macros, not objects -- the standard says so
// (they need only be expressions of type FILE*, and on this toolchain they
// expand to a call into the CRT's __acrt_iob_func).  A module cannot export a
// macro: `import std;` gives you std::fprintf, std::fflush and std::FILE, and
// then no way to name the three streams they are meant to operate on.
//
// So the include is confined to this file, which turns each macro into a
// FUNCTION and exports that.  Everything else in chatwire says
// `chatwire::stdio::out()` and stays free of the preprocessor.
//
// The functions are not `constexpr` and not `inline` on purpose: the expansion
// has to happen in a translation unit that has actually seen <cstdio>, and this
// is the only one that has.
#include <cstdio>

export module chatwire.stdio;
import std;

export namespace chatwire::stdio
{
    /* @brief The standard output stream.  `stdout`, as something with a name. */
    [[nodiscard]] auto out() noexcept -> std::FILE* { return stdout; }

    /* @brief The standard error stream. */
    [[nodiscard]] auto err() noexcept -> std::FILE* { return stderr; }

    /* @brief The standard input stream. */
    [[nodiscard]] auto in() noexcept -> std::FILE* { return stdin; }

    /*
        @brief Points one of those streams at a different file.
        @details
        Here for the same reason the three accessors are: freopen_s is Annex K,
        declared in <stdio.h> at GLOBAL scope rather than in namespace std, so
        `import std;` does not bring it and a caller cannot reach it without the
        include.  Wrapping it keeps that fact in this file with the macros.

        The plain `freopen` would do as well, but it returns the stream and
        loses the reason on failure; freopen_s returns errno_t, which is what
        makes a bool here honest.

        @param stream  What to repoint -- out(), err() or in().
        @param path    The new target.  "CONOUT$" and "CONIN$" for a console.
        @param mode    As fopen: "w", "r", and so on.
        @return true when the stream now refers to `path`.
    */
    auto reopen(std::FILE* const stream, const char* const path,
                const char* const mode) noexcept -> bool
    {
        std::FILE* reopened{ nullptr };
        return ::freopen_s(&reopened, path, mode, stream) == 0;
    }
}
