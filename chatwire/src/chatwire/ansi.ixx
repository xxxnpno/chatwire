export module chatwire.ansi;
import std;

// chatwire.ansi — Minecraft's section-sign colour codes, as ANSI.
//
// Shared by the in-game console and the reference client, because a chat line
// without its colours looks broken to anyone who knows Minecraft, and having two
// copies of a translation table is how they drift.
export namespace chatwire::ansi
{
    /*
        @brief Minecraft's section sign, U+00A7, as the UTF-8 bytes C2 A7.
        @details
        Spelled once, here, because writing it by hand is a trap: a source file
        saved as UTF-8 that then gets its bytes re-encoded turns C2 A7 into
        C3 82 C2 A7, and the extra byte shows up as a stray "A" in front of every
        colour code -- in the game's chat as well as the console.  That happened.
    */
    inline constexpr std::string_view section{ "\xC2\xA7" };

    /* @brief Resets all attributes.  Worth emitting after every line. */
    inline constexpr std::string_view reset{ "\x1b[0m" };

    /*
        @brief Renders §-coded Minecraft text as ANSI escapes.
        @details
        The section sign is U+00A7, which in the UTF-8 chatwire emits is the two
        bytes C2 A7 — so the scan looks for the pair, not for a single char.
        Getting that wrong drops the byte after every colour code.

        Codes 0-9 and a-f are the sixteen colours; l/o/n are bold/italic/
        underline; r resets.  k (obfuscated) and m (strikethrough) have no
        sensible terminal equivalent and are DROPPED rather than approximated —
        showing "magic" text as plain would be a lie about what the server sent.

        Always ends with a reset, so one coloured line cannot bleed into the next.
    */
    [[nodiscard]] inline auto render(const std::string_view text) -> std::string
    {
        static constexpr std::string_view palette[16]{
            "\x1b[30m", "\x1b[34m", "\x1b[32m", "\x1b[36m",
            "\x1b[31m", "\x1b[35m", "\x1b[33m", "\x1b[37m",
            "\x1b[90m", "\x1b[94m", "\x1b[92m", "\x1b[96m",
            "\x1b[91m", "\x1b[95m", "\x1b[93m", "\x1b[97m" };

        std::string out;
        out.reserve(text.size() + 32u);

        for (std::size_t i{ 0 }; i < text.size(); ++i)
        {
            const bool is_section{ static_cast<unsigned char>(text[i]) == 0xC2u
                                   && i + 2 < text.size()
                                   && static_cast<unsigned char>(text[i + 1]) == 0xA7u };
            if (!is_section) { out += text[i]; continue; }

            const char code{ text[i + 2] };
            i += 2;
            if (code >= '0' && code <= '9')      { out += palette[code - '0']; }
            else if (code >= 'a' && code <= 'f') { out += palette[code - 'a' + 10]; }
            else if (code == 'l')                { out += "\x1b[1m"; }
            else if (code == 'o')                { out += "\x1b[3m"; }
            else if (code == 'n')                { out += "\x1b[4m"; }
            else if (code == 'r')                { out += reset; }
            // k / m deliberately dropped.
        }
        out += reset;
        return out;
    }

    /* @brief Strips § codes entirely, the way getUnformattedText does. */
    [[nodiscard]] inline auto strip(const std::string_view text) -> std::string
    {
        std::string out;
        out.reserve(text.size());
        for (std::size_t i{ 0 }; i < text.size(); ++i)
        {
            if (static_cast<unsigned char>(text[i]) == 0xC2u
                && i + 2 < text.size()
                && static_cast<unsigned char>(text[i + 1]) == 0xA7u)
            {
                i += 2;
                continue;
            }
            out += text[i];
        }
        return out;
    }
}
