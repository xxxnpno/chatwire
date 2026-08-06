// chatwire/common.hpp — the standard-library headers chatwire uses.
//
// One place rather than a bespoke list per header: the set is small, every
// header wants most of it, and a single include keeps the ordering rule below
// impossible to get wrong.
//
// ORDERING RULE: standard headers BEFORE any platform header.  <winsock2.h> and
// <windows.h> declare their world inside `extern "C"`, and a std declaration
// first seen from inside that block can pick up C language linkage.  Including
// this first, everywhere, means the standard library is always declared before
// Windows gets a chance to wrap it.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
// <charconv> is how chatwire/config.hpp parses a number out of a config line:
// it reports overflow for the destination's own type, which is what let two
// hand-rolled digit loops and their two different ceilings go away.
#include <charconv>
#include <chrono>
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

// NOT HERE: <meta>, the C++26 reflection header, even though two headers below
// this one use it and everything else in the project is listed above.
//
// It is left to chatwire/json.hpp and chatwire/config.hpp to include for
// themselves, because a header in THIS list is a header every file in the
// project parses -- and clangd 22, which is the language server for this
// codebase, does not ship <meta> at all.  Putting it here made every file in
// chatwire report `'meta' file not found` followed by a few hundred cascading
// "undeclared identifier 'std'" errors, none of them real and none of them
// about the file being edited.  Confined to the two headers that use
// reflection, the same limitation costs two files instead of all of them.
//
// The ordering rule still holds where it matters: both of those headers include
// this one first and <meta> immediately after, so the standard library is still
// fully declared before any platform header can wrap it.
