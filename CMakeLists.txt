cmake_minimum_required(VERSION 3.28)   # CXX_MODULES file sets

project(chatwire
    VERSION      0.1.0
    DESCRIPTION  "Live WebSocket API for Minecraft 1.8.9, built on vmhook"
    LANGUAGES    CXX)

# ---------------------------------------------------------------------------
# C++26 + modules.
#
# The standard is not aspirational here: chatwire is built as a set of named
# modules, and the whole project is one `import chatwire;` away from a host.
# CXX_SCAN_FOR_MODULES needs a generator that understands dyndep -- Ninja or
# Visual Studio.  Makefiles will not do.
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build configuration" FORCE)
endif()

if(NOT WIN32)
    message(FATAL_ERROR
        "chatwire targets Windows: it is injected into a running Minecraft "
        "process and uses Winsock directly.")
endif()

option(CHATWIRE_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)

# ---------------------------------------------------------------------------
# vmhook — vendored, header-only.
#
# Pinned rather than fetched: chatwire depends on invocation working pure-VM
# (vmhook >= 0.6.0), and a floating dependency on a library this project is
# injected alongside is how a game crashes after an unrelated upgrade.
# ---------------------------------------------------------------------------
add_library(vmhook INTERFACE)
add_library(vmhook::vmhook ALIAS vmhook)
target_include_directories(vmhook INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/ext>")

# ---------------------------------------------------------------------------
# The library itself: every .ixx is a module interface unit.
# ---------------------------------------------------------------------------
# The implementation unit is an ordinary source, not part of the CXX_MODULES
# file set: it PROVIDES no module interface, it belongs to one.
add_library(chatwire_lib STATIC src/chatwire_impl.cpp)
add_library(chatwire::chatwire ALIAS chatwire_lib)

target_sources(chatwire_lib
    PUBLIC
        FILE_SET CXX_MODULES FILES
            src/core/log.ixx
            src/core/pump.ixx
            src/core/feature.ixx
            src/core/json.ixx
            src/mapping.ixx
            src/sdk.ixx
            src/ws/websocket.ixx
            src/ws/server.ixx
            src/features/chat.ixx
            src/chatwire.ixx
            src/entry.ixx)

target_link_libraries(chatwire_lib PUBLIC vmhook::vmhook ws2_32)

# GCC 15 segfaults in the `local-fnsummary` GIMPLE pass while instantiating
# std::_Hashtable::try_emplace for vmhook's class registry inside a module TU.
# It is a compiler bug in the interprocedural analysis, not a code defect, and
# -fno-ipa-* is a far smaller hammer than dropping the optimisation level for
# the whole library.  Revisit when GCC 16 lands.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(chatwire_lib PRIVATE -fno-ipa-cp -fno-ipa-sra)
endif()
target_compile_features(chatwire_lib PUBLIC cxx_std_26)

# ---------------------------------------------------------------------------
# The injectable DLL.
# ---------------------------------------------------------------------------
add_library(chatwire SHARED src/dllmain.cpp)
target_link_libraries(chatwire PRIVATE chatwire::chatwire)
set_target_properties(chatwire PROPERTIES
    PREFIX ""                       # chatwire.dll, not libchatwire.dll
    OUTPUT_NAME "chatwire")

# ---------------------------------------------------------------------------
# Warnings.
#
# vmhook is a 24k-line third-party header; its diagnostics are not chatwire's to
# fix, and -Werror on someone else's header only ever produces noise.  So the
# warning set is applied to chatwire's own targets and vmhook is INTERFACE
# (never compiled on its own).
# ---------------------------------------------------------------------------
function(chatwire_apply_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /EHsc /Zc:__cplusplus)
        if(CHATWIRE_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic
            -Wno-unused-function          # vmhook header
            -Wno-unknown-pragmas)
        if(CHATWIRE_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

chatwire_apply_warnings(chatwire_lib)
chatwire_apply_warnings(chatwire)

# ---------------------------------------------------------------------------
# GCC 15 does not put function-local statics from MODULE-ATTACHED inline
# functions into COMDAT sections, so every TU that instantiates one emits its
# own copy and the linker sees a duplicate symbol.  It is a gap in the module
# implementation, not an ODR violation in the code: the definitions are
# genuinely identical, produced from one source, and --allow-multiple-definition
# makes the linker keep the FIRST and point every reference at it.  One object,
# one cache, one mutex -- which is the intended semantics.
#
# Scoped to the final link only.  Revisit when GCC emits these as weak.
# ---------------------------------------------------------------------------
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_link_options(chatwire PRIVATE -Wl,--allow-multiple-definition)
endif()

# Static-link the runtime so the DLL is self-contained.  It is injected into a
# process whose PATH we do not control; a missing libstdc++/libgcc there fails
# the injection with an error that looks nothing like its cause.
if(MINGW)
    target_link_options(chatwire PRIVATE -static -static-libgcc -static-libstdc++)
endif()

# ---------------------------------------------------------------------------
# Tests.
#
# Only the halves that can run without Minecraft: the WebSocket server, the
# framing, and the JSON.  They are also the halves most likely to be wrong -- a
# hand-written RFC 6455 codec is exactly the kind of code that looks right and
# mishandles a length boundary.  The Minecraft side needs a live game and is
# verified by injecting.
# ---------------------------------------------------------------------------
option(CHATWIRE_BUILD_TESTS "Build the no-Minecraft test suite" ON)

if(CHATWIRE_BUILD_TESTS)
    enable_testing()
    add_executable(chatwire_test_server tests/test_server.cpp)
    target_link_libraries(chatwire_test_server PRIVATE chatwire::chatwire)
    chatwire_apply_warnings(chatwire_test_server)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_link_options(chatwire_test_server PRIVATE -Wl,--allow-multiple-definition)
    endif()
    add_test(NAME server COMMAND chatwire_test_server)
endif()

message(STATUS "chatwire ${PROJECT_VERSION} - C++${CMAKE_CXX_STANDARD}, modules ON")
