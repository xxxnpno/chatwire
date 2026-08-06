#pragma once

// chatwire.core.json — just enough JSON, with no dependency.
//
// ===========================================================================
// SCOPE
// ===========================================================================
// chatwire's wire format is flat objects of strings, numbers and booleans:
//
//     {"cmd":"chat.send","text":"hello"}
//     {"type":"chat","formatted":"§ahi","plain":"hi","at":1234}
//
// That is all this parses and all it emits.  It is NOT a general JSON library:
// there is no nested-object access, no arrays-of-objects, no number tower.
// Pulling in nlohmann/json for this would add a 24k-line header to a DLL that
// gets injected into someone's game, to read four string fields.
//
// What it IS careful about is being fed hostile input, because the socket is
// reachable by anything on the machine:
//
//   * escape handling never reads past the end;
//   * an unterminated string is a parse failure, not a scan into the heap;
//   * nesting is refused outright rather than mis-parsed;
//   * output escaping covers the control range, so a chat line containing a
//     quote or a newline cannot forge a second JSON field.
//
// That last one matters most: chat text is ATTACKER-CONTROLLED (any player on
// the server can say anything), and it flows straight into the JSON this emits.
#include "chatwire/common.hpp"
namespace chatwire::json
{
    /*
        @brief Escapes `text` into a JSON string body (without the quotes).
        @details
        Escapes the two characters JSON requires (`"` and `\`), the shorthand
        control characters, and every remaining C0 control as \u00XX.  Bytes
        >= 0x80 pass through untouched: the input is UTF-8 and JSON strings are
        UTF-8, so re-encoding would corrupt the section signs Minecraft colour
        codes are made of.
    */
    [[nodiscard]] inline auto escape(const std::string_view text)
        -> std::string
    {
        std::string out;
        out.reserve(text.size() + 16u);
        for (const char raw : text)
        {
            const auto c{ static_cast<unsigned char>(raw) };
            switch (c)
            {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            default:
                if (c < 0x20u)
                {
                    static constexpr char hex[]{ "0123456789abcdef" };
                    out += "\\u00";
                    out += hex[(c >> 4) & 0x0Fu];
                    out += hex[c & 0x0Fu];
                }
                else
                {
                    out += raw;
                }
                break;
            }
        }
        return out;
    }

    /* @brief `"key":"value"` with both escaped. */
    [[nodiscard]] inline auto field(const std::string_view key, const std::string_view value)
        -> std::string
    {
        return std::format("\"{}\":\"{}\"", escape(key), escape(value));
    }

    /* @brief `"key":123` — numbers are emitted unquoted. */
    [[nodiscard]] inline auto field(const std::string_view key, const std::int64_t value)
        -> std::string
    {
        return std::format("\"{}\":{}", escape(key), value);
    }

    /*
        @brief `"key":true`
        @details
        CONSTRAINED to actual bool, and that constraint is load-bearing.

        Unconstrained, this overload SWALLOWS STRING LITERALS: `const char*` to
        `bool` is a standard pointer-to-bool conversion, while `const char*` to
        `std::string_view` is user-defined, so overload resolution prefers bool.
        `field("type", "chat")` then silently emits `"type":true` — valid JSON,
        completely wrong, and invisible until a consumer wonders why a string
        field is a boolean.  It shipped exactly that way for one build here.

        requires std::same_as<bool> removes the overload from consideration for
        anything that is not already a bool, so a literal has nowhere to go but
        the string_view overload.
    */
    template<typename bool_type>
        requires std::same_as<std::remove_cvref_t<bool_type>, bool>
    [[nodiscard]] inline auto field(const std::string_view key, const bool_type value)
        -> std::string
    {
        // `{}` on a bool is "true"/"false" -- std::format's default for bool is
        // exactly JSON's spelling, so the ternary this replaces is gone rather
        // than moved into the argument.
        return std::format("\"{}\":{}", escape(key), value);
    }

    /*
        @brief `"key":"value"` for a string LITERAL.
        @details
        Exists so a literal is unambiguously a string even before the constraint
        above is considered: an exact match beats every conversion, so this is
        chosen outright.  Belt and braces on the trap described above, because
        the failure mode is silent.
    */
    template<std::size_t n>
    [[nodiscard]] inline auto field(const std::string_view key, const char (&value)[n])
        -> std::string
    {
        return std::format("\"{}\":\"{}\"", escape(key),
                           escape(std::string_view{ value, n - 1u }));
    }

    /*
        @brief Reads a top-level string field out of a flat JSON object.
        @details
        Scans for `"key"`, skips to the value, and unescapes it.  Returns
        nullopt when the key is absent, when the value is not a string, or when
        the object is malformed — all three are "the client sent something I
        cannot use", which the caller reports as one error.

        Only TOP-LEVEL keys match.  A key inside a nested object would be found
        by a naive scan and silently treated as top-level, so nesting depth is
        tracked and anything below depth 1 is skipped.
    */
    [[nodiscard]] inline auto get_string(const std::string_view object,
                                         const std::string_view key)
        -> std::optional<std::string>
    {
        std::size_t i{ 0 };
        int         depth{ 0 };

        const auto skip_ws{ [&]() noexcept
        {
            while (i < object.size()
                   && (object[i] == ' ' || object[i] == '\t'
                       || object[i] == '\n' || object[i] == '\r'))
            {
                ++i;
            }
        } };

        // Reads a quoted string starting at object[i] == '"'.  Returns nullopt
        // on an unterminated string rather than running to the end of the view.
        const auto read_string{ [&]() -> std::optional<std::string>
        {
            if (i >= object.size() || object[i] != '"') { return std::nullopt; }
            ++i;
            std::string out;
            while (i < object.size())
            {
                const char c{ object[i] };
                if (c == '"') { ++i; return out; }
                if (c == '\\')
                {
                    // An escape needs one more byte; a trailing backslash is
                    // malformed, and reading it would step past the end.
                    if (i + 1 >= object.size()) { return std::nullopt; }
                    const char esc{ object[i + 1] };
                    switch (esc)
                    {
                    case '"':  out += '"';  break;
                    case '\\': out += '\\'; break;
                    case '/':  out += '/';  break;
                    case 'n':  out += '\n'; break;
                    case 'r':  out += '\r'; break;
                    case 't':  out += '\t'; break;
                    case 'b':  out += '\b'; break;
                    case 'f':  out += '\f'; break;
                    case 'u':
                    {
                        // \uXXXX needs four hex digits.  We decode the BMP range
                        // to UTF-8; surrogate pairs are passed through as the
                        // replacement character rather than half-decoded, since
                        // chatwire's own clients send plain text.
                        if (i + 5 >= object.size()) { return std::nullopt; }
                        std::uint32_t cp{ 0 };
                        for (std::size_t k{ 2 }; k < 6; ++k)
                        {
                            const char h{ object[i + k] };
                            cp <<= 4;
                            if (h >= '0' && h <= '9')      { cp |= static_cast<std::uint32_t>(h - '0'); }
                            else if (h >= 'a' && h <= 'f') { cp |= static_cast<std::uint32_t>(h - 'a' + 10); }
                            else if (h >= 'A' && h <= 'F') { cp |= static_cast<std::uint32_t>(h - 'A' + 10); }
                            else { return std::nullopt; }
                        }
                        if (cp < 0x80u)
                        {
                            out += static_cast<char>(cp);
                        }
                        else if (cp < 0x800u)
                        {
                            out += static_cast<char>(0xC0u | (cp >> 6));
                            out += static_cast<char>(0x80u | (cp & 0x3Fu));
                        }
                        else if (cp >= 0xD800u && cp <= 0xDFFFu)
                        {
                            out += "\xEF\xBF\xBD";        // U+FFFD
                        }
                        else
                        {
                            out += static_cast<char>(0xE0u | (cp >> 12));
                            out += static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
                            out += static_cast<char>(0x80u | (cp & 0x3Fu));
                        }
                        i += 4;                            // the four hex digits
                        break;
                    }
                    default:
                        return std::nullopt;               // unknown escape
                    }
                    i += 2;
                    continue;
                }
                out += c;
                ++i;
            }
            return std::nullopt;                            // unterminated
        } };

        skip_ws();
        if (i >= object.size() || object[i] != '{') { return std::nullopt; }
        ++i;
        depth = 1;

        while (i < object.size())
        {
            skip_ws();
            if (i >= object.size()) { break; }

            const char c{ object[i] };
            if (c == '}') { --depth; ++i; if (depth == 0) { break; } continue; }
            if (c == '{' || c == '[') { ++depth; ++i; continue; }
            if (c == ']') { --depth; ++i; continue; }
            if (c == ',' || c == ':') { ++i; continue; }

            if (c == '"')
            {
                const std::size_t key_start{ i };
                auto candidate{ read_string() };
                if (!candidate) { return std::nullopt; }

                // Only a key at depth 1 followed by ':' is a top-level key.
                skip_ws();
                const bool is_key{ i < object.size() && object[i] == ':' };
                if (!is_key || depth != 1)
                {
                    (void)key_start;
                    continue;                                // it was a value
                }
                ++i;                                         // past ':'
                skip_ws();

                if (*candidate == key)
                {
                    if (i < object.size() && object[i] == '"') { return read_string(); }
                    return std::nullopt;                     // present, not a string
                }

                // Wrong key: skip its value so a nested object cannot be
                // mistaken for more top-level keys.
                if (i < object.size() && object[i] == '"')
                {
                    if (!read_string()) { return std::nullopt; }
                }
                continue;
            }

            ++i;                                             // number / literal byte
        }
        return std::nullopt;
    }

    /*
        @brief Wraps `body` in braces.
        @details
        The one string in chatwire that is NOT built with std::format, and
        deliberately: the format string would be `"{{{}}}"` -- an escaped brace,
        a replacement field, an escaped brace -- which is harder to read than
        the thing it produces, and it would give up the move.  `body` is every
        field of a response already joined, so it is the longest string here and
        the one worth not copying.

        Everything else in this file formats.  This is the exception, and the
        reason is that it is a WRAP rather than a substitution: there is nothing
        to interpolate, only two characters to put on the ends.
    */
    [[nodiscard]] inline auto object(std::string body)
        -> std::string
    {
        return "{" + std::move(body) + "}";
    }
}
