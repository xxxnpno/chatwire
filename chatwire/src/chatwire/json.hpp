#pragma once

// chatwire.core.json — just enough JSON, with no dependency.
//
// ===========================================================================
// SCOPE
// ===========================================================================
// chatwire's wire format is objects of strings, numbers, booleans and arrays:
//
//     {"cmd":"chat.send","text":"hello"}
//     {"type":"chat","formatted":"§ahi","plain":"hi","at":1234}
//     {"count":2,"players":[{"name":"alpha","uuid":"..."}]}
//
// That is all this emits, and rather less than that is what it parses.  It is
// NOT a general JSON library: there is no nested-object ACCESS and no number
// tower.  Pulling in nlohmann/json for this would add a 24k-line header to a
// DLL that gets injected into someone's game, to read four string fields.
//
// ===========================================================================
// A MESSAGE IS A STRUCT, AND THE FIELD NAMES ARE THE MEMBER NAMES
// ===========================================================================
// The writer is generated from the type.  A caller declares the shape:
//
//     struct status_result
//     {
//         std::string_view version;
//         std::int64_t     port;
//         bool             can_call;
//     };
//
//     json::object(status_result{ .version = "0.3.0", .port = 24455,
//                                 .can_call = true })
//         -> {"version":"0.3.0","port":24455,"can_call":true}
//
// `object()` walks std::meta::nonstatic_data_members_of and splices each member
// out, so the KEY is the member's identifier and the SPELLING is chosen by the
// member's type.  Neither is written by hand anywhere.
//
// What that replaced, at every call site in the project:
//
//     json::object(std::format("{},{},{}",
//         json::field("version",  chatwire::version),
//         json::field("port",     static_cast<std::int64_t>(port)),
//         json::field("can_call", can_call)))
//
// Three separate things a human had to keep true there, none of them checkable:
// the count of `{}` had to equal the count of arguments, each quoted key had to
// stay spelled the way the README says, and each value had to be cast to a type
// one of the `field` overloads accepted.  The reflective version cannot get any
// of the three wrong, because there is no second list to keep in step -- the
// struct IS the message, and adding a field to the wire format is adding a
// member.
//
// It also retires a trap that shipped here for one build.  `field()` was an
// overload set, and an unconstrained `bool` overload SWALLOWS STRING LITERALS:
// `const char*` to `bool` is a standard conversion while `const char*` to
// `std::string_view` is user-defined, so `field("type", "chat")` resolved to the
// bool overload and emitted `"type":true` -- valid JSON, completely wrong, and
// invisible until a consumer wondered why a string field was a boolean.  It had
// to be fixed with a `requires std::same_as<bool>` and a second overload for
// literals.  Overload resolution is not involved any more: a member's type is
// known exactly, and `if constexpr` picks its spelling with no conversions in
// the picture.
//
// ===========================================================================
// WHAT IS DELIBERATELY *NOT* REFLECTED
// ===========================================================================
// The READER, below, stays a hand-written scanner over the raw bytes.  It is
// the half that faces hostile input, its per-call-site error messages ("missing
// or non-string 'text'") are what a client author actually reads, and a feature
// pulls one or two known keys rather than filling a schema.  Generating a
// binder for that would add a layer to the careful half of this file in order
// to save nothing.  Reflection is used where there was DUPLICATION to remove,
// not everywhere it would fit.
//
// ===========================================================================
// HOSTILE INPUT
// ===========================================================================
// The socket is reachable by anything on the machine, and chat text is
// ATTACKER-CONTROLLED -- any player on the server can say anything, and it
// flows straight into the JSON this emits.  So:
//
//   * escape handling never reads past the end;
//   * an unterminated string is a parse failure, not a scan into the heap;
//   * nesting is refused outright rather than mis-parsed;
//   * output escaping covers the control range, so a chat line containing a
//     quote or a newline cannot forge a second JSON field.
//
// The last one is unchanged by any of the above: every string VALUE still goes
// through escape().  What reflection removed is the escaping of KEYS, which the
// old `field()` did on every field of every message -- a key is now a C++
// identifier by construction, and there is no identifier escape() would alter.
#include "chatwire/common.hpp"

// Deliberately here rather than in common.hpp: see the note at the bottom of
// that file.  Immediately after it, so the ordering rule it exists for still
// holds.
#include <meta>
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

    /*
        @brief JSON that is already JSON, written through untouched.
        @details
        The one escape hatch, and it has exactly one job: the dispatcher wraps a
        feature's finished reply in `{"ok":true,"result":<that>}`, and by then
        the reply is a string of JSON rather than a struct.  Without this it
        would be a string VALUE and arrive quoted and escaped -- the result
        object turned into a lump of text the client would have to parse a
        second time.

        Named so that a reader of a message struct can see which member is the
        unchecked one.  Nothing else in the project uses it.
    */
    struct verbatim
    {
        std::string_view text{};
    };

    namespace detail
    {
        /*
            @brief The non-static data members of `object_type`, at compile time.
            @details
            Wrapped in a consteval function rather than held in a constexpr
            variable because nonstatic_data_members_of returns a vector ALLOCATED
            DURING CONSTANT EVALUATION.  Such an allocation may not survive to
            run time, so naming the vector in a constexpr variable is an error
            ("non-transient allocation").  std::define_static_array copies it
            into static storage and hands back a span, which may.
        */
        template<typename object_type>
        consteval auto members_of()
        {
            return std::define_static_array(
                std::meta::nonstatic_data_members_of(^^object_type,
                                                     std::meta::access_context::current()));
        }

        // Declared before write_object because the two are mutually recursive:
        // an object holds values, and a value may be an object (or an array of
        // them -- `players` is exactly that).
        template<typename value_type>
        auto write(std::string& out, const value_type& value) -> void;

        /*
            @brief Appends `value`'s members as `"key":value` pairs, no braces.
            @details
            Brace-less because `object()` may be given SEVERAL structs to emit as
            one flat object -- see the note there.  `separate` carries whether
            anything has been written yet, so the comma between two structs is
            placed by the same rule as the comma between two members.
        */
        template<typename object_type>
        auto write_members(std::string& out, const object_type& value, bool& separate) -> void
        {
            template for (constexpr auto member : members_of<object_type>())
            {
                if (separate) { out += ','; }
                separate = true;

                // define_static_string, not identifier_of directly: the key is
                // needed at RUN time, and this is what promises the characters
                // are still there then rather than being a view into the
                // constant evaluation that produced them.
                //
                // Not escaped, and it cannot need to be: a C++ identifier has no
                // quote, no backslash and no control character in it.  The old
                // field() ran escape() over every key of every message to
                // establish something the language had already guaranteed.
                constexpr std::string_view key{
                    std::define_static_string(std::meta::identifier_of(member)) };

                out += '"';
                out += key;
                out += "\":";
                write(out, value.[:member:]);
            }
        }

        /*
            @brief Appends one value in its JSON spelling.
            @details
            The order of these branches is the whole of the type mapping, and
            two of them are order-dependent:

              * `bool` FIRST, because a bool is also an integral type and would
                otherwise be emitted as 0 or 1.
              * string-like BEFORE range, because std::string is a range of char
                and would otherwise become an array of numbers.

            An unhandled type is a compile error naming the type, which is the
            behaviour worth having: the alternative is a member silently
            omitted from the wire format.
        */
        template<typename value_type>
        auto write(std::string& out, const value_type& value) -> void
        {
            using type = std::remove_cvref_t<value_type>;

            if constexpr (std::same_as<type, verbatim>)
            {
                out += value.text;
            }
            else if constexpr (std::same_as<type, bool>)
            {
                out += value ? "true" : "false";
            }
            else if constexpr (std::convertible_to<const type&, std::string_view>)
            {
                out += '"';
                out += escape(value);
                out += '"';
            }
            else if constexpr (std::integral<type>)
            {
                out += std::format("{}", value);
            }
            else if constexpr (std::ranges::input_range<type>)
            {
                out += '[';
                bool separate{ false };
                for (const auto& element : value)
                {
                    if (separate) { out += ','; }
                    separate = true;
                    write(out, element);
                }
                out += ']';
            }
            else if constexpr (std::is_class_v<type>)
            {
                out += '{';
                bool separate{ false };
                write_members(out, value, separate);
                out += '}';
            }
            else
            {
                static_assert(false, "no JSON spelling for this member's type");
            }
        }
    }

    /*
        @brief What `object()` accepts: a struct describing a message.
        @details
        Spelled out rather than left to fail deep inside the writer, and it
        earns its keep on ONE case: `object()` used to take a std::string of
        pre-joined fields and wrap it in braces.  A std::string is a class type,
        so an un-ported call site would still compile -- and quietly emit
        `"{\"sent\":true}"`, a quoted string where an object belongs.  Excluding
        string-like types makes that a compile error instead.
    */
    template<typename object_type>
    concept describes_object =
        std::is_class_v<std::remove_cvref_t<object_type>>
        && !std::convertible_to<const std::remove_cvref_t<object_type>&, std::string_view>
        && !std::ranges::input_range<std::remove_cvref_t<object_type>>
        && !std::same_as<std::remove_cvref_t<object_type>, verbatim>;

    /*
        @brief One JSON object, from one or more structs describing it.
        @details
        Several structs are emitted as one FLAT object, their fields in
        argument order:

            json::object(chat::stats(), world::stats())
                -> {"lines_seen":3,"sent":1,"worlds_entered":2}

        which is what `system.stats` needs: the counters belong to whichever
        feature keeps them, and the answer is one object rather than one object
        per feature.  The arrangement this replaced had every feature return its
        fields WITHOUT braces -- a JSON fragment, valid nowhere, that only the
        host knew how to join -- and each such function carried a paragraph
        explaining why it was shaped like that.  A feature now returns a struct
        like everything else, and the joining is a parameter pack.
    */
    template<describes_object... object_types>
    [[nodiscard]] inline auto object(const object_types&... parts) -> std::string
    {
        std::string out{ "{" };
        bool        separate{ false };
        (detail::write_members(out, parts, separate), ...);
        out += '}';
        return out;
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
}
