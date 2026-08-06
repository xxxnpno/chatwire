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

#include "chatwire/module.hpp"

#include <cstdio>

namespace chatwire::config
{
    struct settings
    {
        /* @brief TCP port on 127.0.0.1.  0 means "the caller said nothing". */
        std::uint16_t port{ 0 };
        /*
            @brief Attach a console window to the game.
            @details
            OFF by default, matching the injector.  chatwire's interface is the
            websocket; the console is a convenience for watching chat, and on a
            game that already has one (Lunar, or any java.exe launch) chatwire's
            output has to share it with the game's own logger.  Opt in.
        */
        bool console{ false };
        /* @brief Show the start-up trace, not just chat and problems. */
        bool verbose{ false };
        /*
            @brief Seconds to wait for Minecraft's classes.  0 means "unset".
            @details
            Covers the gap between "the game's process exists" and "Minecraft's
            classes are loaded", which is the whole of a launcher's start-up if
            chatwire is run against a game still sitting on its login screen.
            That can outlast any fixed default, which is why this is reachable
            from the environment.
        */
        std::uint32_t timeout_seconds{ 0 };
    };

    /*
        @brief The config file beside the chatwire library that is asking.
        @details
        Beside the LIBRARY, not beside the game and not in the working
        directory: the injector knows where it put the library and nothing else
        about the process it is injecting into.  Empty when the path cannot be
        determined, which consume() reads as "no file" -- defaults, not failure.
    */
    [[nodiscard]] inline auto path_for_self() -> std::string
    {
        return chatwire::module::sibling("chatwire.cfg");
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
            std::array<char, 1024> buffer{};
            std::size_t            read{ 0 };

            // Plain stdio rather than each platform's file API.  This is a
            // kilobyte of key=value written by us and read by us seconds later;
            // there is nothing here that CreateFile bought over fopen.
            if (std::FILE* const file{ std::fopen(path.c_str(), "rb") })
            {
                read = std::fread(buffer.data(), 1u, buffer.size() - 1u, file);
                (void)std::fclose(file);
            }

            // Deleted whether or not it parsed, and BEFORE it is used: these are
            // the settings for THIS injection, and a file left behind would
            // silently apply them to the next one.  A delete that fails changes
            // nothing this run.
            (void)std::remove(path.c_str());
            if (read == 0u) { return out; }

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
                else if (key == "timeout")
                {
                    std::uint32_t parsed{ 0 };
                    bool          digits{ !value.empty() };
                    for (const char c : value)
                    {
                        if (c < '0' || c > '9') { digits = false; break; }
                        parsed = parsed * 10u + static_cast<std::uint32_t>(c - '0');
                        if (parsed > 86400u) { digits = false; break; }
                    }
                    if (digits) { out.timeout_seconds = parsed; }
                }
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
            // The two flags are formatted as INTEGERS, not as bools: read()
            // above tests `value != "0"`, and std::format's default spelling of
            // a bool is "true"/"false", which that test would read as true
            // either way -- a config that could set a flag but never clear it.
            const std::string text{ std::format("port={}\nconsole={}\nverbose={}\ntimeout={}\n",
                                                s.port,
                                                s.console ? 1 : 0,
                                                s.verbose ? 1 : 0,
                                                s.timeout_seconds) };

            std::FILE* const file{ std::fopen(path.c_str(), "wb") };
            if (file == nullptr) { return false; }

            const std::size_t written{ std::fwrite(text.data(), 1u, text.size(), file) };
            const bool        flushed{ std::fclose(file) == 0 };
            return flushed && written == text.size();
        }
        catch (...) { return false; }
    }
}
