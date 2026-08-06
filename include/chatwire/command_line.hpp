#pragma once

// chatwire.command_line — turning a typed line into a command and its arguments.
//
// ===========================================================================
// WHY THIS IS ITS OWN HEADER
// ===========================================================================
// It is the only part of the `commands` feature that is pure text: no JVM, no
// vmhook, no socket.  Everything else in that feature needs a running game to
// mean anything, and `sdk.hpp` -- which the feature includes -- is the one
// header in the project allowed to pull in vmhook's 24,000 lines.
//
// Splitting these three functions out is what lets them be TESTED.  They decide
// whether `/ping a b` reaches a plugin and what it is told the arguments were,
// which is the part a plugin author actually depends on and the part a refactor
// could quietly change without any test noticing.  A test that had to include
// the feature would have to include vmhook, which is both slow and the thing
// the layering exists to prevent.
//
// It is also shared with chatwire-mock, which has to make the same decisions in
// order to be a useful stand-in.  It used to hold a hand-copied normalise() --
// two implementations of one rule, which is exactly the drift a mock is
// supposed to be defended against.
#include "chatwire/common.hpp"
namespace chatwire::command_line
{
    /* Long enough for any plausible command, short enough to be a name. */
    inline constexpr std::size_t max_name_length{ 64 };

    /*
        @brief The command name a typed line invokes, or empty.
        @details
        `/ping a b` -> "ping".  A line that is not a slash command, or is just
        "/", names nothing.  The leading slash is dropped because that is
        Minecraft's syntax rather than part of the name -- a plugin registers
        `ping`, and registering `/ping` means the same thing.

        The result VIEWS `line`, so it lives exactly as long as the line does.
    */
    [[nodiscard]] inline auto invoked_name(const std::string_view line) noexcept
        -> std::string_view
    {
        if (line.size() < 2u || line.front() != '/') { return {}; }
        const std::string_view rest{ line.substr(1) };
        const std::size_t end{ rest.find_first_of(" \t") };
        return end == std::string_view::npos ? rest : rest.substr(0, end);
    }

    /*
        @brief The arguments after the command name.
        @details
        Whitespace-separated with empties dropped, which is what every Minecraft
        command parser does and what npnoqol's command::get_arguments does -- so
        a plugin author gets what they expect rather than what is easiest here.
        `/ping   a  b ` is therefore ["a","b"], not ["","a","","b",""].

        Quoting is deliberately ABSENT.  A quoting scheme is a small language,
        every one of them differs, and guessing wrong would silently mangle
        somebody's argument.  The event carries `raw` alongside these, so a
        plugin that wants its own rules has the untouched line to apply them to.
    */
    [[nodiscard]] inline auto arguments(const std::string_view line) -> std::vector<std::string>
    {
        std::vector<std::string> out;
        const std::string_view name{ invoked_name(line) };
        if (name.empty()) { return out; }

        std::size_t i{ 1u + name.size() };          // past "/name"
        while (i < line.size())
        {
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) { ++i; }
            const std::size_t start{ i };
            while (i < line.size() && line[i] != ' ' && line[i] != '\t') { ++i; }
            if (i > start) { out.emplace_back(line.substr(start, i - start)); }
        }
        return out;
    }

    /*
        @brief Normalises a name a client asked to register.
        @details
        One leading slash is tolerated and removed, because half of all plugin
        authors will type it and refusing that would be pedantry.  Everything
        else is REFUSED rather than repaired: a name with a space in it could
        never be invoked -- invoked_name would stop at the space -- and silently
        truncating it would register something the author did not ask for and
        did not get told about.

        @return the name, viewing `name`, or empty when it is not usable.
    */
    [[nodiscard]] inline auto normalise(std::string_view name) noexcept -> std::string_view
    {
        if (!name.empty() && name.front() == '/') { name.remove_prefix(1); }
        if (name.empty() || name.size() > max_name_length) { return {}; }
        for (const char c : name)
        {
            // A second '/' would make the name unreachable the same way a space
            // would; a control character has no business in one at all.
            if (c == ' ' || c == '\t' || c == '/'
                || static_cast<unsigned char>(c) < 0x20u)
            {
                return {};
            }
        }
        return name;
    }
}
