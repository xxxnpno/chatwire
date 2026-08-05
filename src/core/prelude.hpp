// prelude.hpp — the identical global-module-fragment preamble every chatwire
// module includes FIRST.
//
// ===========================================================================
// WHY THIS EXISTS
// ===========================================================================
// GCC 15's module implementation merges declarations that arrive from a global
// module fragment with declarations it already has, and it is order-sensitive
// about it.  When two modules include different subsets of the standard library
// in different orders, the same entity can end up declared two ways, and the
// second module to see it fails with things like:
//
//     error: conflicting 'noexcept' specifier for imported declaration
//            'int atexit(void (*)())'
//     error: mismatching abi tags for 'std::...' with tags '"cxx11"'
//     error: conflicting language linkage for imported declaration
//
// None of those are bugs in the code that triggers them.  They are the compiler
// failing to reconcile two views of the same declaration.
//
// The reliable fix is to give every module the SAME view: one header, included
// first, everywhere.  A module that needs something extra includes it after,
// but the shared core is always identical and always first.
//
// ===========================================================================
// ORDERING RULES BAKED IN HERE
// ===========================================================================
//   * <cstdlib> before anything else.  Namespace-scope and function-local
//     statics with dynamic initialisation make GCC reference `atexit`; if the
//     first declaration it sees is its own builtin rather than the library's,
//     any module that imports a module which saw the library's version
//     conflicts.  Declaring it up front, identically, in every TU removes the
//     disagreement.
//   * Standard headers before ANY platform header.  <winsock2.h> and
//     <windows.h> declare their world inside `extern "C"`, and a std
//     declaration first seen from inside that block acquires C language linkage.
//
// Keep this list minimal but stable.  Adding a header is cheap; reordering or
// removing one can resurrect the errors above in a module that looks unrelated.
#pragma once

// MUST BE FIRST.  See the ordering rules above.
#include <cstdlib>

#include <array>
#include <atomic>
#include <concepts>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>
