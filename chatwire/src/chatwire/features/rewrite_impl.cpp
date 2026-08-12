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
// <atomic> and <memory> for the rewrite rule set; harmless elsewhere.
#include <atomic>
#include <memory>

// The state the interface only DECLARES.
//
// Each of these was an inline function with a function-local static, which is
// the construct-on-first-use idiom and exactly right for a header.  In a module
// GCC 16.2 emits that static into the module's object AND into every consumer's,
// and `-Wl,--allow-multiple-definition` -- which the link needs anyway -- then
// silently gives each consumer its own copy.  Duplicated CODE is harmless;
// duplicated STATE meant the feature registry was written by one copy and read
// from another, so every feature registered and none was found.
//
// A .cpp is compiled once, so its statics exist once.  Nothing else changes:
// these are the same bodies, still constructed on first use, still deliberately
// leaked so a detour that outlives shutdown cannot use freed memory.

module chatwire.features.rewrite;

namespace chatwire::features::detail
{
    auto rules() noexcept -> std::atomic<std::shared_ptr<const rule_set>>&
    {
        static auto* const live{
            new std::atomic<std::shared_ptr<const rule_set>>{
                std::make_shared<const rule_set>() } };
        return *live;
    }
}
