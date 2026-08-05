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
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>
