module;

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <meta>

export module chatwire.reflect;

// chatwire.core.reflect — the one reflection primitive the project shares.
//
// ===========================================================================
// WHY THIS FILE EXISTS
// ===========================================================================
// Three places generate code from a struct's members: the JSON writer, the
// config reader/writer, and the mapping verifier.  All three need the same
// thing -- the non-static data members of a type, usable at run time -- and all
// three had, or would have had, their own copy of the same six lines with the
// same paragraph of explanation attached.  A third copy is where that stops
// being a coincidence.
//
// ===========================================================================
// WHY IT IS NOT A CONSTEXPR VARIABLE
// ===========================================================================
// `nonstatic_data_members_of` returns a std::vector ALLOCATED DURING CONSTANT
// EVALUATION.  Such an allocation may not survive to run time, so naming the
// vector in a constexpr variable is an error -- GCC 16 says "non-transient
// allocation".  `std::define_static_array` copies it into static storage and
// hands back a span, which may.
//
// ===========================================================================
// WHAT INCLUDING THIS COSTS
// ===========================================================================
// <meta>, and therefore clangd.  clangd 22 does not ship the header, so every
// file that reaches this one reports `'meta' file not found` and a few hundred
// cascading errors that are all artefacts of that one miss.  See the note at
// the bottom of common.hpp: the rule is that <meta> stays confined to the files
// that actually reflect, and this file does not widen that set -- it is
// included by exactly the files that were including <meta> already, plus the
// one new feature that reflects.

// Immediately after common.hpp, so the ordering rule that file exists for still
// holds: the whole standard library is declared before any platform header can
// wrap it in `extern "C"`.

export namespace chatwire::reflect
{
    /*
        @brief The non-static data members of `object_type`, usable at run time.
        @details
        `access_context::current()` means "what a member of THIS namespace can
        see", which for the structs the project reflects over -- all of them
        public aggregates -- is all of them.  Spelling it is not optional: the
        parameter has no default.
    */
    template<typename object_type>
    consteval auto members_of()
    {
        return std::define_static_array(
            std::meta::nonstatic_data_members_of(^^object_type,
                                                 std::meta::access_context::current()));
    }

    /*
        @brief A member's identifier as a string that outlives the evaluation.
        @details
        `identifier_of` is consteval and returns a view into the constant
        evaluation that produced it, so it cannot simply be stored and read
        later.  define_static_string is what promises the characters are still
        there at run time.

        Every caller wants exactly this, and getting it wrong is not a compile
        error at the point of use -- it is a dangling view.
    */
    /*
        @brief The TYPE of a member, as a reflection a caller can splice.
        @details
        Here rather than at the call site because this unit is the ONLY one that
        may include <meta>.  Two module fragments that both include it break the
        header's own internals: GCC 16.2 fails to look up
        `std::meta::reflect_constant_array` from inside `define_static_array`,
        with the error pointing into libstdc++ rather than at either of ours.

        So the rule this file enforces is narrow and mechanical: `std::meta` is
        named in reflect.ixx and nowhere else, and everything anyone needs comes
        back through these three functions.  Splice it with `[:type_of(m):]`.
    */
    template<std::meta::info member>
    [[nodiscard]] consteval auto type_of_member() -> std::meta::info
    {
        return std::meta::type_of(member);
    }

    template<std::meta::info member>
    [[nodiscard]] consteval auto identifier() -> std::string_view
    {
        return std::define_static_string(std::meta::identifier_of(member));
    }
}
