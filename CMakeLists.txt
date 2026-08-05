cmake_minimum_required(VERSION 3.20)

project(chatwire
    VERSION      0.2.0
    DESCRIPTION  "Live WebSocket API for Minecraft 1.8.9, built on vmhook"
    LANGUAGES    CXX)

# ---------------------------------------------------------------------------
# C++23, header-based.
#
# chatwire was originally built as C++26 named modules.  That is gone: GCC 15
# and Clang 19 both fall over on this shape -- segfaults instantiating vmhook
# inside a module, a hard failure for any TU that both imports a module and
# includes <windows.h>, duplicate symbols from module-attached inline statics.
# None of it was a defect in this code, and all of it was time spent on the
# build rather than on the library.
#
# Headers build everywhere, today, with no workarounds.  The layering the
# modules forced -- one boundary for vmhook, one for Winsock -- was worth
# keeping, and survives as ordinary include discipline.
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build configuration" FORCE)
endif()

if(NOT WIN32)
    message(FATAL_ERROR
        "chatwire targets Windows: it is injected into a running Minecraft "
        "process and uses Winsock directly.")
endif()

option(CHATWIRE_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)
option(CHATWIRE_BUILD_TOOLS        "Build the injector, client and mock" ON)
option(CHATWIRE_BUILD_TESTS        "Build the no-Minecraft test suite"   ON)

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
# The library.  Header-based, with one .cpp for the bodies that should not be
# duplicated into every consumer (start/stop and the wiring they use).
# ---------------------------------------------------------------------------
add_library(chatwire_lib STATIC src/chatwire.cpp)
add_library(chatwire::chatwire ALIAS chatwire_lib)

target_include_directories(chatwire_lib PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_link_libraries(chatwire_lib PUBLIC vmhook::vmhook ws2_32)
target_compile_features(chatwire_lib PUBLIC cxx_std_23)

# ---------------------------------------------------------------------------
# Warnings.
#
# vmhook is a 24k-line third-party header; its diagnostics are not chatwire's to
# fix, and -Werror on someone else's header only ever produces noise.  So the
# warning set applies to chatwire's own targets, and vmhook is INTERFACE (never
# compiled on its own).
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

# ---------------------------------------------------------------------------
# The injectable DLL.
# ---------------------------------------------------------------------------
add_library(chatwire SHARED src/dllmain.cpp)
target_link_libraries(chatwire PRIVATE chatwire::chatwire)
set_target_properties(chatwire PROPERTIES
    PREFIX ""                       # chatwire.dll, not libchatwire.dll
    OUTPUT_NAME "chatwire")
chatwire_apply_warnings(chatwire)

# Self-contained: injected into a process whose PATH we do not control, where a
# missing libstdc++/libgcc fails the injection with an error that looks nothing
# like its cause.
if(MINGW)
    target_link_options(chatwire PRIVATE -static -static-libgcc -static-libstdc++)
endif()

# ---------------------------------------------------------------------------
# Tools.
# ---------------------------------------------------------------------------
if(CHATWIRE_BUILD_TOOLS)
    # The injector talks to the OS, not to chatwire, so it links nothing of ours.
    add_executable(chatwire-inject tools/injector.cpp)
    chatwire_apply_warnings(chatwire-inject)

    # The mock serves chatwire's protocol with synthetic chat, so a consumer can
    # be built and tested without a Minecraft client open.  It links the REAL
    # server and JSON, which is what keeps its wire format from drifting.
    add_executable(chatwire-mock tools/mock.cpp)
    target_link_libraries(chatwire-mock PRIVATE chatwire::chatwire)
    chatwire_apply_warnings(chatwire-mock)

    # The reference client speaks the protocol over a socket like any other
    # consumer would -- it links nothing of ours on purpose.  If the reference
    # client needed privileged access to the library, the API would not be one.
    add_executable(chatwire-client tools/client.cpp)
    target_link_libraries(chatwire-client PRIVATE ws2_32)
    chatwire_apply_warnings(chatwire-client)

    if(MINGW)
        foreach(tool chatwire-inject chatwire-mock chatwire-client)
            target_link_options(${tool} PRIVATE -static -static-libgcc -static-libstdc++)
        endforeach()
    endif()
endif()

# ---------------------------------------------------------------------------
# Tests: the halves that run without Minecraft -- the WebSocket server, the
# framing, and the JSON.  They are also the halves most likely to be wrong: a
# hand-written RFC 6455 codec is exactly the kind of code that looks right and
# mishandles a length boundary.  The Minecraft side needs a live game.
# ---------------------------------------------------------------------------
if(CHATWIRE_BUILD_TESTS)
    enable_testing()
    add_executable(chatwire_test_server tests/test_server.cpp)
    target_link_libraries(chatwire_test_server PRIVATE chatwire::chatwire)
    chatwire_apply_warnings(chatwire_test_server)
    add_test(NAME server COMMAND chatwire_test_server)
endif()

message(STATUS "chatwire ${PROJECT_VERSION} - C++${CMAKE_CXX_STANDARD}, header-based")
