export module chatwire.reflect;
import std;

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
// clangd, and nothing else any more.  clangd 22 implements no part of P2996, so
// every file that reflects reports parse errors on `^^`, on a splice and on
// `template for` -- all of them artefacts of the tool rather than of the code,
// and all of them checked by compiling instead.
//
// What it USED to cost was an <meta> include that had to be confined to the
// files that actually reflect and placed before any platform header.  `import
// std;` carries std::meta, so there is no header to confine and no order to get
// right: a unit that reflects imports std like every other unit does.

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
