#pragma once

// chatwire.module — where am I on disk?
//
// chatwire needs its own path for one reason: the injector leaves a settings
// file NEXT TO the library, and the library has to find it without being told
// where it was loaded from.  See config.hpp for why the settings travel as a
// file at all.
//
// Windows does not let a DLL ask this directly of itself; it answers "which
// module contains this address?", so the question is asked by passing an
// address inside the library.  That is what `marker` below is for -- its own
// address is the question.  A string literal would not do: literals can be
// merged, folded into another section, or shared with a different module
// entirely, and the answer would be that module's path.
#include "chatwire/common.hpp"

#include <windows.h>

namespace chatwire::module
{
    namespace detail
    {
        /* Its ADDRESS is the payload; the value is never read. */
        inline int marker{ 0 };
    }

    /*
        @brief The full path of the shared library this code is linked into.
        @details
        Empty when it cannot be determined, which callers must treat as "no
        config file" rather than as a failure -- a chatwire loaded by some means
        that leaves no path behind should still start on its defaults.

        This deliberately takes the module handle by ADDRESS lookup rather
        than from a DllMain-captured HMODULE.  The captured handle was
        fine but had to be threaded through every caller, and it does not exist
        at all in a build with no DllMain -- the test binaries, for one.

        UNCHANGED_REFCOUNT is not an optimisation: GetModuleHandleEx without it
        takes a reference that is never released, which would pin the module in
        the process forever.  chatwire already declines to unload itself, but it
        should decline on purpose rather than by accident.
    */
    [[nodiscard]] inline auto own_path() noexcept -> std::string
    {
        try
        {
            ::HMODULE self{ nullptr };
            if (!::GetModuleHandleExA(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                        | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<::LPCSTR>(&detail::marker), &self))
            {
                return {};
            }

            char        buffer[MAX_PATH]{};
            const ::DWORD n{ ::GetModuleFileNameA(self, buffer, MAX_PATH) };
            if (n == 0u || n >= MAX_PATH) { return {}; }
            return std::string{ buffer, n };
        }
        catch (...) { return {}; }
    }

    /*
        @brief `<the directory this library is in>/<name>`.
        @details
        Splits on both separators: a Windows path can legally contain forward
        slashes, and one that arrived from a config file or a launcher often
        does.
    */
    [[nodiscard]] inline auto sibling(const std::string_view name) noexcept -> std::string
    {
        try
        {
            const std::string self{ own_path() };
            if (self.empty()) { return {}; }

            const std::size_t slash{ self.find_last_of("\\/") };
            if (slash == std::string::npos) { return {}; }
            return self.substr(0, slash + 1u) + std::string{ name };
        }
        catch (...) { return {}; }
    }
}
