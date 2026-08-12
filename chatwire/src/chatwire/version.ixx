export module chatwire.version;
import std;

// chatwire.version — the two constants everything else is allowed to know.
//
// These lived in chatwire.api until that module grew start()'s body, and then
// they could not stay: chatwire.api imports every feature so it can wire them
// up, and chatwire.features.system imports chatwire.api for `version` to put in
// its status reply.  As a header that was a harmless back-reference.  As modules
// it is a CYCLE, and the build says so in as many words:
//
//     CMake Error: Circular dependency detected in the C++ module import graph.
//     See modules named: "chatwire.api", "chatwire.entry",
//     "chatwire.features.system"
//
// A module graph has to be acyclic, which is a real constraint and, here, a
// useful one: it asked what a feature ACTUALLY needed from the root module, and
// the answer was two constants and nothing else.  So they live at the bottom of
// the graph where a leaf can reach them, and chatwire.api re-exports them so a
// host that imports chatwire.api still finds `chatwire::version` exactly where
// it always was.
export namespace chatwire
{
    /* @brief chatwire's own version. */
    inline constexpr std::string_view version{ "0.3.0" };

    /*
        @brief The default port.  Loopback only; see chatwire.ws.server for why
               that is a default worth defending rather than a placeholder.
    */
    inline constexpr std::uint16_t default_port{ 24455 };
}
