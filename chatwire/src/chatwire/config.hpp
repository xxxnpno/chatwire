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
//
// ===========================================================================
// THE KEYS ARE THE MEMBER NAMES, IN BOTH DIRECTIONS
// ===========================================================================
// `write` and `consume` are both generated from `settings` by walking its
// members, so a setting is declared once, as a member, and the reader and the
// writer cannot disagree about it.
//
// They used to be able to, and the file said so.  `write` carried this note:
//
//     The two flags are formatted as INTEGERS, not as bools: read() above tests
//     `value != "0"`, and std::format's default spelling of a bool is
//     "true"/"false", which that test would read as true either way -- a config
//     that could set a flag but never clear it.
//
// That is a real bug, described in a comment, held off by a human remembering
// the comment.  It cannot happen now for a structural reason: the spelling of a
// bool is chosen ONCE, in one `if constexpr`, and the same branch is what the
// reader tests against.  Adding a setting is adding a member -- there is no
// second place to add it to, and no third place to keep in step.
//
// Renaming a member renames its key, which is safe HERE and would not be
// somewhere else: chatwire.dll is carried inside chatwire.exe as a resource
// (see CMakeLists.txt), so the writer and the reader are always the same build,
// and the file lives for about a second between them.
#pragma once

#include "chatwire/common.hpp"

// Deliberately here rather than in common.hpp: see the note at the bottom of
// that file.  Before module.hpp, which reaches windows.h, so the ordering rule
// still holds.
#include <meta>

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
        @brief The longest wait `timeout` may ask for: one day.
        @details
        A sanity bound rather than a limit anyone would reach.  It is the only
        rule about a setting that its TYPE does not already state, which is why
        it is the only one written down.
    */
    inline constexpr std::uint32_t max_timeout_seconds{ 86400 };

    namespace detail
    {
        /*
            @brief `settings`' members, at compile time.
            @details
            Through define_static_array for the same reason json.hpp does it:
            nonstatic_data_members_of allocates during constant evaluation, and
            that allocation may not be named by a constexpr variable.
        */
        consteval auto members_of()
        {
            return std::define_static_array(
                std::meta::nonstatic_data_members_of(^^settings,
                                                     std::meta::access_context::current()));
        }

        /*
            @brief Applies one `key=value` line to the setting it names.
            @details
            Unknown keys are IGNORED rather than refused: a config written by a
            newer injector than the library reading it cannot happen here (they
            ship as one file), but a truncated or half-written file can, and
            defaults are a better answer than refusing to start.

            No `continue` in the expansion below, and that is not a style
            choice: an expansion statement is not a loop, so `break` and
            `continue` do not belong to it.
        */
        inline auto apply(settings& out, const std::string_view key,
                          const std::string_view value) noexcept -> void
        {
            template for (constexpr auto member : members_of())
            {
                constexpr std::string_view name{
                    std::define_static_string(std::meta::identifier_of(member)) };
                if (key == name)
                {
                    using field_type = [:std::meta::type_of(member):];

                    if constexpr (std::same_as<field_type, bool>)
                    {
                        // The ONE place a bool's spelling is decided.  write()
                        // below emits whatever this test wants to see.
                        out.[:member:] = (value != "0");
                    }
                    else
                    {
                        // from_chars rather than a hand-rolled digit loop, of
                        // which this file had two that differed only in their
                        // ceiling.  It refuses a sign, and reports overflow for
                        // the member's OWN type -- so `port=99999` is out of
                        // range because a port is a std::uint16_t, rather than
                        // because somebody typed 65535 into a comparison.
                        //
                        // `ptr == last` is the other half: from_chars stops at
                        // the first character it cannot use and calls that
                        // success, so without it `timeout=12x` would be 12.
                        field_type parsed{};
                        const auto* const first{ value.data() };
                        const auto* const last{ value.data() + value.size() };
                        const auto result{ std::from_chars(first, last, parsed) };
                        if (result.ec == std::errc{} && result.ptr == last)
                        {
                            out.[:member:] = parsed;
                        }
                    }
                }
            }
        }
    }

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

                detail::apply(out, line.substr(0, eq), line.substr(eq + 1u));
            }

            // The one bound that is not a property of a type.  A port's ceiling
            // is std::uint16_t's and needs saying nowhere; a day is just the
            // longest wait that could be meant, so it is stated here where a
            // reader can see it rather than buried in a parse loop.
            if (out.timeout_seconds > max_timeout_seconds)
            {
                out.timeout_seconds = 0u;
            }
        }
        catch (...)
        {
            // A malformed config is not worth refusing to start over.
        }
        return out;
    }

    /*
        @brief Writes a config file.  Used by the injector.
        @details
        Every member, in declaration order, spelled the way detail::apply reads
        it.  There is no list of keys here to fall out of step with the one over
        there, because there is no list of keys anywhere -- see the note at the
        top of this file for the bug that arrangement used to hold at arm's
        length with a comment.
    */
    [[nodiscard]] inline auto write(const std::string& path, const settings& s) noexcept -> bool
    {
        try
        {
            std::string text;
            template for (constexpr auto member : detail::members_of())
            {
                constexpr std::string_view name{
                    std::define_static_string(std::meta::identifier_of(member)) };
                using field_type = [:std::meta::type_of(member):];

                if constexpr (std::same_as<field_type, bool>)
                {
                    // An INTEGER, because that is what detail::apply's
                    // `value != "0"` test wants.  std::format's default
                    // spelling of a bool is "true"/"false", which that test
                    // reads as true either way -- a config that could set a
                    // flag but never clear it.  The two halves are in one file
                    // and one `if constexpr` apart, so the next reader can see
                    // in one glance that they agree.
                    text += std::format("{}={}\n", name, s.[:member:] ? 1 : 0);
                }
                else
                {
                    text += std::format("{}={}\n", name, s.[:member:]);
                }
            }

            std::FILE* const file{ std::fopen(path.c_str(), "wb") };
            if (file == nullptr) { return false; }

            const std::size_t written{ std::fwrite(text.data(), 1u, text.size(), file) };
            const bool        flushed{ std::fclose(file) == 0 };
            return flushed && written == text.size();
        }
        catch (...) { return false; }
    }
}
