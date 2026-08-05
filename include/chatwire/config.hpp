// chatwire/config.hpp — settings handed from the injector to the injected DLL.
//
// ===========================================================================
// WHY A FILE
// ===========================================================================
// The injector has flags; the DLL wakes up inside a process the injector did not
// start.  Those two facts do not meet: a process's environment is fixed at
// creation, so `--port 9000` cannot be delivered as CHATWIRE_PORT to a game that
// is already running -- an earlier version of this tool printed an apologetic
// note saying exactly that.
//
// So the injector writes the settings to a file beside chatwire.dll, and the DLL
// reads it during start-up and deletes it.  Deleting matters: a stale file would
// silently apply last week's flags to today's injection.
//
// The format is one `key=value` per line.  Not JSON, not INI -- this file is
// written and read by the same two programs, is never edited by hand, and lives
// for about a second.
#pragma once

#include "chatwire/common.hpp"

#include <windows.h>

namespace chatwire::config
{
    struct settings
    {
        /* @brief TCP port on 127.0.0.1.  0 means "the caller said nothing". */
        std::uint16_t port{ 0 };
        /* @brief Attach a console window to the game. */
        bool console{ true };
        /* @brief Show the start-up trace, not just chat and problems. */
        bool verbose{ false };
    };

    namespace detail
    {
        /* @brief `<folder of this module>\chatwire.cfg`. */
        inline auto path_beside(const HMODULE module_handle) -> std::string
        {
            char buffer[MAX_PATH]{};
            const DWORD n{ ::GetModuleFileNameA(module_handle, buffer, MAX_PATH) };
            if (n == 0u || n >= MAX_PATH) { return {}; }

            std::string path{ buffer, n };
            const std::size_t slash{ path.find_last_of("\\/") };
            if (slash == std::string::npos) { return {}; }
            return path.substr(0, slash + 1u) + "chatwire.cfg";
        }
    }

    /* @brief The config file that belongs to `module_handle`'s directory. */
    [[nodiscard]] inline auto path_for(const HMODULE module_handle) -> std::string
    {
        return detail::path_beside(module_handle);
    }

    /*
        @brief Reads and then DELETES the config file.
        @details
        Consuming it is the point: these are the settings for THIS injection, and
        leaving the file behind would apply them to the next one silently.  A
        missing file is the normal case (someone injected by other means) and
        yields defaults rather than an error.
    */
    [[nodiscard]] inline auto consume(const std::string& path) noexcept -> settings
    {
        settings out{};
        if (path.empty()) { return out; }

        try
        {
            const HANDLE file{ ::CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                             nullptr, OPEN_EXISTING,
                                             FILE_ATTRIBUTE_NORMAL, nullptr) };
            if (file == INVALID_HANDLE_VALUE) { return out; }

            std::array<char, 1024> buffer{};
            DWORD read{ 0 };
            const BOOL ok{ ::ReadFile(file, buffer.data(),
                                      static_cast<DWORD>(buffer.size() - 1u), &read, nullptr) };
            ::CloseHandle(file);
            (void)::DeleteFileA(path.c_str());
            if (!ok || read == 0u) { return out; }

            const std::string_view text{ buffer.data(), read };
            std::size_t line_start{ 0 };
            while (line_start < text.size())
            {
                std::size_t line_end{ text.find('\n', line_start) };
                if (line_end == std::string_view::npos) { line_end = text.size(); }

                std::string_view line{ text.substr(line_start, line_end - line_start) };
                line_start = line_end + 1u;
                while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                {
                    line.remove_suffix(1u);
                }

                const std::size_t eq{ line.find('=') };
                if (eq == std::string_view::npos) { continue; }
                const std::string_view key{ line.substr(0, eq) };
                const std::string_view value{ line.substr(eq + 1u) };

                if (key == "port")
                {
                    std::uint32_t parsed{ 0 };
                    bool          digits{ !value.empty() };
                    for (const char c : value)
                    {
                        if (c < '0' || c > '9') { digits = false; break; }
                        parsed = parsed * 10u + static_cast<std::uint32_t>(c - '0');
                        if (parsed > 65535u) { digits = false; break; }
                    }
                    if (digits) { out.port = static_cast<std::uint16_t>(parsed); }
                }
                else if (key == "console") { out.console = (value != "0"); }
                else if (key == "verbose") { out.verbose = (value != "0"); }
            }
        }
        catch (...)
        {
            // A malformed config is not worth refusing to start over.
        }
        return out;
    }

    /* @brief Writes a config file.  Used by the injector. */
    [[nodiscard]] inline auto write(const std::string& path, const settings& s) noexcept -> bool
    {
        try
        {
            std::string text;
            text += "port=" + std::to_string(s.port) + "\n";
            text += std::string{ "console=" } + (s.console ? "1" : "0") + "\n";
            text += std::string{ "verbose=" } + (s.verbose ? "1" : "0") + "\n";

            const HANDLE file{ ::CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr) };
            if (file == INVALID_HANDLE_VALUE) { return false; }

            DWORD written{ 0 };
            const BOOL ok{ ::WriteFile(file, text.data(),
                                       static_cast<DWORD>(text.size()), &written, nullptr) };
            ::CloseHandle(file);
            return ok != 0 && written == text.size();
        }
        catch (...) { return false; }
    }
}
