# 3.28 is the floor for C++20 MODULES: below it CMake does not scan sources for
# imports, so nothing is ordered, no module mapper is passed, and every consumer
# fails with "failed to read compiled module".
cmake_minimum_required(VERSION 3.28)

project(chatwire
    VERSION      0.3.0
    DESCRIPTION  "Live WebSocket API for Minecraft 1.8.9, built on vmhook"
    LANGUAGES    CXX)

# ---------------------------------------------------------------------------
# C++26, header-based.
#
# chatwire was originally built as C++26 named MODULES.  That is still gone, and
# the reasons have not changed: GCC 15 and Clang 19 both fell over on that shape
# -- segfaults instantiating vmhook inside a module, a hard failure for any TU
# that both imports a module and includes <windows.h>, duplicate symbols from
# module-attached inline statics.  Headers build with no workarounds, and the
# layering the modules forced -- one boundary for vmhook, one for Winsock --
# survives as ordinary include discipline.
#
# What HAS changed is the language level.  chatwire is C++26 for one feature it
# actually uses: static reflection (P2996).  Every JSON object this project puts
# on the wire is now a struct whose field names ARE its member names, written by
# chatwire/json.hpp walking `std::meta::nonstatic_data_members_of`.  What that
# replaced was a positional `std::format("{},{},{}", field(...), field(...),
# field(...))` per message -- where the count of `{}` and the count of arguments
# were two facts a human had to keep equal, and where a mistyped key was a wire
# format change nothing could catch.  See the top of chatwire/json.hpp.
#
# Reflection is a HARD requirement rather than a nicety with a fallback: a
# fallback would be a second implementation of the wire format, and two
# implementations of one rule is the exact drift this codebase refuses
# elsewhere.  It is detected below and refused at configure time.
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build configuration" FORCE)
endif()

# ---------------------------------------------------------------------------
# Windows only.
#
# Not a gap in a port -- chatwire needs to load a library into a game that is
# ALREADY RUNNING, and that is `CreateRemoteThread` + `LoadLibrary` here.  The
# equivalents elsewhere are a debugger attaching to a live game (ptrace, on
# Linux) or something a hardened-runtime JVM refuses outright (task_for_pid, on
# macOS).  Refused at configure time rather than failing later in a wall of
# missing headers.
# ---------------------------------------------------------------------------
if(NOT WIN32)
    message(FATAL_ERROR "chatwire is Windows-only.")
endif()

# Threads are used by every target here.  THREADS_PREFER_PTHREAD_FLAG asks for
# -pthread rather than -lpthread: the two are not equivalent, because the flag
# also sets the preprocessor state that makes libstdc++ pick its threaded
# implementations rather than silently linking single-threaded stubs.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Build options.  Declared BEFORE the <print> check below, which is guarded on
# them: a guard that reads an option not yet declared is silently always false.
# ---------------------------------------------------------------------------
option(CHATWIRE_WARNINGS_AS_ERRORS "Treat warnings as errors"              OFF)
option(CHATWIRE_BUILD_TOOLS        "Build chatwire.exe (Windows)"          ON)
option(CHATWIRE_EMBED_DLL          "Carry chatwire.dll inside chatwire.exe" ON)

# ---------------------------------------------------------------------------
# Where things land.
#
# The build directory should contain the thing you came for.  Configure into
# `build/etc` and the executables appear in `build/`, with every generated file
# CMake and Ninja need -- the cache, the rules, the object tree, the logs --
# kept one level down in `build/etc` where nobody has to look at them:
#
#     cmake -S . -B build/etc -G Ninja
#     cmake --build build/etc
#     build/chatwire.exe
#
# It has to be this way round.  CMakeCache.txt, CMakeFiles/ and build.ninja are
# not relocatable -- they live in the binary directory by definition -- so the
# only way to keep them out of a directory is to make that directory the PARENT
# of the binary one.  Hence `etc`: the name is the agreed marker, and it is the
# only thing this branch keys on.
#
# Configuring straight into `build` still works and puts everything together,
# which is the ordinary CMake behaviour and what a tool that invokes cmake
# itself will expect.
#
# The DLL is an INTERMEDIATE on Windows now: it is carried inside the exe, so it
# goes wherever the object files go rather than sitting next to the binary
# pretending it is something you need to keep.
# ---------------------------------------------------------------------------
get_filename_component(CHATWIRE_BINARY_LEAF "${CMAKE_BINARY_DIR}" NAME)
if(CHATWIRE_BINARY_LEAF STREQUAL "etc")
    get_filename_component(CHATWIRE_OUTPUT_DIR "${CMAKE_BINARY_DIR}/.." ABSOLUTE)
else()
    set(CHATWIRE_OUTPUT_DIR "${CMAKE_BINARY_DIR}")
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CHATWIRE_OUTPUT_DIR}")
# Import libraries and static archives are build inputs, never deliverables.
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# The DLL's own destination is set on the target, not here: on Windows a shared
# library's .dll is its RUNTIME artifact (the .lib is the "library" one), so the
# global LIBRARY_OUTPUT_DIRECTORY would not move it and the global RUNTIME one
# would drag every executable along with it.
if(WIN32 AND CHATWIRE_EMBED_DLL)
    set(CHATWIRE_DLL_OUTPUT_DIR "${CMAKE_BINARY_DIR}/lib")
else()
    set(CHATWIRE_DLL_OUTPUT_DIR "${CHATWIRE_OUTPUT_DIR}")
endif()

# ---------------------------------------------------------------------------
# Static reflection, which is the one C++26 feature this project leans on and
# the reason it is C++26 at all.
#
# Compiled rather than inferred from the compiler VERSION, because the two do
# not line up: GCC 16 has it behind a FLAG (-freflection) and says
# `'std::meta' has not been declared` without it, which reads like a missing
# header rather than a missing flag.  Clang 22 accepts -std=c++26 and has no
# <meta> at all.  So the probe is the whole feature -- a reflection, a splice,
# and the non-transient-allocation dance that std::define_static_array exists
# for -- and not just the header's presence.
#
# CMAKE_REQUIRED_FLAGS carries the flag into the check and nothing else does:
# check_cxx_source_compiles honours CMAKE_CXX_STANDARD (CMP0067) but knows
# nothing about a flag that standard needs.
#
# The flag lands on chatwire's own targets, never globally: a global
# -freflection would also reach every OTHER try_compile CMake runs -- including
# the <print> check below, which does not set the standard the flag requires and
# would fail with "-freflection only supported with -std=c++26", diagnosing a
# missing <print> that is right there.
# ---------------------------------------------------------------------------
include(CheckCXXSourceCompiles)

set(CHATWIRE_REFLECTION_PROGRAM [[
#include <meta>
#include <string_view>
struct probe { int first; bool second; };
consteval auto names() {
    return std::define_static_array(
        std::meta::nonstatic_data_members_of(^^probe,
                                             std::meta::access_context::current()));
}
int main() {
    constexpr probe value{ 7, true };
    int seen = 0;
    template for (constexpr auto member : names()) {
        std::string_view key{ std::define_static_string(std::meta::identifier_of(member)) };
        seen += static_cast<int>(key.size()) + static_cast<int>(sizeof(value.[:member:]));
    }
    return seen == 0 ? 1 : 0;
}
]])

set(CMAKE_REQUIRED_QUIET ON)
set(CMAKE_REQUIRED_FLAGS "-freflection")
check_cxx_source_compiles("${CHATWIRE_REFLECTION_PROGRAM}" CHATWIRE_REFLECTION_FLAGGED)
unset(CMAKE_REQUIRED_FLAGS)

if(CHATWIRE_REFLECTION_FLAGGED)
    set(CHATWIRE_REFLECTION_OPTIONS "-freflection")
else()
    # Some compiler may one day have it on by default; ask before refusing.
    check_cxx_source_compiles("${CHATWIRE_REFLECTION_PROGRAM}" CHATWIRE_REFLECTION_PLAIN)
    if(CHATWIRE_REFLECTION_PLAIN)
        set(CHATWIRE_REFLECTION_OPTIONS "")
    else()
        message(FATAL_ERROR
            "chatwire needs C++26 static reflection (P2996), and this compiler "
            "(${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}) has none. "
            "GCC 16 or newer, which is what implements it today -- Clang 22 does "
            "not. On Windows, MSYS2 does not ship GCC 16 yet; the winlibs builds "
            "do (https://winlibs.com), and are a self-contained directory you "
            "point CMAKE_CXX_COMPILER at:\n"
            "  -DCMAKE_CXX_COMPILER=C:/tools/mingw64/bin/g++.exe\n"
            "There is no non-reflection build. The wire format is generated from "
            "the structs that describe it, and a fallback would be a second "
            "implementation of it.")
    endif()
endif()
unset(CMAKE_REQUIRED_QUIET)

# ---------------------------------------------------------------------------
# <print>, which is the one C++23 library feature this project actually leans on
# and the one most likely to be missing.
#
# The tools print with std::println.  Three things vary and all three have bitten
# a real build:
#
#   * the header may not exist at all (Apple Clang lags libc++ by a year or so,
#     and the system compiler on a current macOS may simply not have it);
#   * on libstdc++ the terminal path lives in a SEPARATE library, libstdc++exp,
#     and without it the link fails on __open_terminal / __write_to_terminal --
#     a link error that names symbols no source file mentions;
#   * which of those is true is not predictable from the compiler VERSION, so it
#     is compiled rather than assumed.
#
# Detected once, here, so a missing <print> is a clear message at configure time
# instead of five hundred lines of template output later.
#
# Only when something that PRINTS is being built.  The library itself does not
# use <print> -- it reports through chatwire/log.hpp -- so
# `-DCHATWIRE_BUILD_TOOLS=OFF` genuinely drops the requirement, and checking
# anyway would refuse a configuration that would have worked.
# ---------------------------------------------------------------------------
if(CHATWIRE_BUILD_TOOLS)
    # CheckCXXSourceCompiles is already included, by the reflection probe above.
    set(CHATWIRE_PRINT_PROGRAM "#include <print>\nint main() { std::println(\"{}\", 1); }")

    set(CMAKE_REQUIRED_QUIET ON)
    check_cxx_source_compiles("${CHATWIRE_PRINT_PROGRAM}" CHATWIRE_PRINT_PLAIN)

    if(NOT CHATWIRE_PRINT_PLAIN)
        set(CMAKE_REQUIRED_LIBRARIES stdc++exp)
        check_cxx_source_compiles("${CHATWIRE_PRINT_PROGRAM}" CHATWIRE_PRINT_WITH_EXP)
        unset(CMAKE_REQUIRED_LIBRARIES)

        if(CHATWIRE_PRINT_WITH_EXP)
            set(CHATWIRE_PRINT_LIBRARY stdc++exp)
        else()
            message(FATAL_ERROR
                "chatwire.exe needs a working C++23 <print>, and this "
                "compiler (${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}) "
                "has none. GCC 14+, LLVM/Clang 18+ with its own libc++, or "
                "MSVC 19.37+. On macOS the system Apple Clang is usually too old: "
                "`brew install llvm` and configure with "
                "-DCMAKE_CXX_COMPILER=$(brew --prefix llvm)/bin/clang++. "
                "Or build just the library, which needs none of it: "
                "-DCHATWIRE_BUILD_TOOLS=OFF.")
        endif()
    endif()
    unset(CMAKE_REQUIRED_QUIET)
endif()


# ---------------------------------------------------------------------------
# vmhook — vendored, header-only.
#
# Pinned rather than fetched: chatwire depends on invocation working pure-VM,
# and on vmhook::attach_current_thread, without which a Java call would once
# again have to happen inside a detour and the pump would have to come back.  A
# floating dependency on a library this project is injected alongside is how a
# game crashes after an unrelated upgrade.
# ---------------------------------------------------------------------------
# vmhook is a C++26 MODULE now, not a header: it is compiled here as an interface
# unit and consumers `import vmhook;`.  A STATIC library rather than INTERFACE,
# because there is an object to produce.
add_library(vmhook STATIC)
add_library(vmhook::vmhook ALIAS vmhook)
target_sources(vmhook PUBLIC
    FILE_SET CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/chatwire/ext/vmhook"
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/chatwire/ext/vmhook/vmhook.ixx")
target_compile_features(vmhook PUBLIC cxx_std_26)
# GCC needs the flag on EVERY translation unit that imports, not only on the
# interface units.  PUBLIC so consumers inherit it.
target_compile_options(vmhook PUBLIC -fmodules)

# ---------------------------------------------------------------------------
# The library.  Header-based, with one .cpp for the bodies that should not be
# duplicated into every consumer (start/stop and the wiring they use).
# ---------------------------------------------------------------------------
add_library(chatwire_lib STATIC chatwire/src/chatwire.cpp)

# EVERY header is a module interface unit now.  CMake scans them, works out the
# order and builds them before whatever imports them; there is no list to keep
# in dependency order by hand.
target_sources(chatwire_lib PUBLIC
    FILE_SET CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/chatwire/src"
    FILES
        chatwire/src/chatwire/reflect.ixx
        chatwire/src/chatwire/crypto.ixx
        chatwire/src/chatwire/json.ixx
        chatwire/src/chatwire/log.ixx
        chatwire/src/chatwire/ansi.ixx
        chatwire/src/chatwire/terminal.ixx
        chatwire/src/chatwire/module.ixx
        chatwire/src/chatwire/net.ixx
        chatwire/src/chatwire/config.ixx
        chatwire/src/chatwire/command_line.ixx
        chatwire/src/chatwire/console.ixx
        chatwire/src/chatwire/mapping.ixx
        chatwire/src/chatwire/feature.ixx
        chatwire/src/chatwire/chatwire.ixx
        chatwire/src/chatwire/sdk.ixx
        chatwire/src/chatwire/ws/websocket.ixx
        chatwire/src/chatwire/ws/server.ixx
        chatwire/src/chatwire/features/chat.ixx
        chatwire/src/chatwire/features/commands.ixx
        chatwire/src/chatwire/features/mapping.ixx
        chatwire/src/chatwire/features/rewrite.ixx
        chatwire/src/chatwire/features/scoreboard.ixx
        chatwire/src/chatwire/features/system.ixx
        chatwire/src/chatwire/features/world.ixx
        chatwire/src/entry.ixx)

# GCC 16.2 emits a function-local `static` from an inline function into the
# module's object AND into every consumer's, so the linker sees each one twice.
# It happens inside libstdc++'s own std::format as well, so no source change
# avoids it; the definitions are identical, which is what makes letting the
# linker pick one safe.  Remove this when GCC stops doing it.
target_link_options(chatwire_lib PUBLIC "-Wl,--allow-multiple-definition")
add_library(chatwire::chatwire ALIAS chatwire_lib)

target_include_directories(chatwire_lib PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/chatwire/src>")
target_link_libraries(chatwire_lib PUBLIC vmhook::vmhook Threads::Threads)
target_compile_features(chatwire_lib PUBLIC cxx_std_26)

# PUBLIC, and that is not tidiness: chatwire/json.hpp is part of the public
# surface and its writer is a template, so the reflection in it is compiled in
# the CONSUMER's translation unit, not in this library's.  A consumer built
# without the flag gets `'std::meta' has not been declared` from a header it
# only meant to include.
target_compile_options(chatwire_lib PUBLIC ${CHATWIRE_REFLECTION_OPTIONS})

target_link_libraries(chatwire_lib PUBLIC ws2_32 bcrypt)

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
add_library(chatwire SHARED chatwire/src/dllmain.cpp)
target_link_libraries(chatwire PRIVATE chatwire::chatwire)
set_target_properties(chatwire PROPERTIES
    PREFIX ""                       # chatwire.dll/.so/.dylib, never libchatwire
    OUTPUT_NAME "chatwire"
    # Both, because which one applies is platform-dependent: RUNTIME is the
    # .dll on Windows, LIBRARY is the .so / .dylib everywhere else.
    RUNTIME_OUTPUT_DIRECTORY "${CHATWIRE_DLL_OUTPUT_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${CHATWIRE_DLL_OUTPUT_DIR}")
chatwire_apply_warnings(chatwire)

# Self-contained: loaded into a process whose library search path we do not
# control, where a missing libstdc++/libgcc fails with an error that looks
# nothing like its cause.
if(MINGW)
    target_link_options(chatwire PRIVATE -static -static-libgcc -static-libstdc++)
endif()

# ---------------------------------------------------------------------------
# chatwire.exe — the library, plus the thing that puts it into the game.
#
# The only binary that ships.  The consumer side is a socket: see the README,
# and mcp/ for an MCP server.  Neither needs a build step, and a client that had
# to be compiled would be a poor advertisement for an API whose whole claim is
# "anything that speaks WebSocket".
# ---------------------------------------------------------------------------
if(CHATWIRE_BUILD_TOOLS)
    # The injector talks to the OS, not to chatwire -- but it does share the
    # config format, so it needs the include path (header-only, no link).
    # The target is chatwire_exe and the FILE is chatwire.exe: two CMake
    # targets cannot share a name, but chatwire.dll and chatwire.exe are
    # different files and may.
    add_executable(chatwire_exe chatwire/src/injector.cpp)
    set_target_properties(chatwire_exe PROPERTIES OUTPUT_NAME "chatwire")
    target_include_directories(chatwire_exe PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/chatwire/src")
    # It does not LINK chatwire_lib, so it does not inherit the flag from it --
    # but it includes chatwire/config.hpp, whose reader and writer are both
    # generated from the settings struct.  Same flag, stated again because the
    # usage requirement has nowhere else to travel from.
    target_compile_options(chatwire_exe PRIVATE ${CHATWIRE_REFLECTION_OPTIONS})
    chatwire_apply_warnings(chatwire_exe)

    # -------------------------------------------------------------------
    # The library, carried INSIDE the executable.
    #
    # A Windows resource rather than a generated C array, because the
    # linker just copies the bytes in: a 3 MB DLL turned into a source
    # file is a 20 MB source file, and every rebuild of the library makes
    # the compiler chew through it again.
    #
    # The .rc names an absolute path in the BUILD tree, which the source
    # tree cannot know, so it is configured at generate time.  Backslashes
    # are doubled first: an .rc string escapes them the way C does, so a
    # raw Windows path would turn \t into a tab.
    # -------------------------------------------------------------------
    if(CHATWIRE_EMBED_DLL)
        enable_language(RC)

        set(CHATWIRE_DLL_RC_PATH "${CHATWIRE_DLL_OUTPUT_DIR}/chatwire.dll")
        string(REPLACE "/" "\\\\" CHATWIRE_DLL_RC_PATH "${CHATWIRE_DLL_RC_PATH}")
        configure_file("${CMAKE_CURRENT_SOURCE_DIR}/chatwire/src/chatwire.rc.in"
                       "${CMAKE_CURRENT_BINARY_DIR}/chatwire.rc" @ONLY)

        target_sources(chatwire_exe PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/chatwire.rc")

        # Two separate things, and BOTH are needed.  add_dependencies orders
        # the two targets so the DLL exists before the resource compiler
        # opens it; OBJECT_DEPENDS re-runs the resource compiler when the
        # DLL's CONTENT changes, which ordering alone does not do -- without
        # it, editing the library and rebuilding would relink an exe still
        # carrying yesterday's bytes.  That is the exact stale-payload bug
        # embedding the DLL was meant to make impossible.
        # A plain path, not $<TARGET_FILE:chatwire>: OBJECT_DEPENDS is
        # consumed by the generator before generator expressions are
        # evaluated, so a genex here becomes a dependency on a file
        # literally named "$<TARGET_FILE:chatwire>" and Ninja stops with
        # "missing and no known rule to make it".
        add_dependencies(chatwire_exe chatwire)
        set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/chatwire.rc"
            PROPERTIES OBJECT_DEPENDS "${CHATWIRE_DLL_OUTPUT_DIR}/chatwire.dll")
    endif()

    # <print>, and whatever it turned out to need; see the top of this file.
    if(CHATWIRE_PRINT_LIBRARY)
        target_link_libraries(chatwire_exe PRIVATE ${CHATWIRE_PRINT_LIBRARY})
    endif()

    # The injector imports chatwire.config to write the handover file, so it
    # needs the target that OWNS that module rather than a copy of the header.
    target_link_libraries(chatwire_exe PRIVATE chatwire::chatwire)

    # Self-contained for the same reason the library is: this is a single
    # file somebody is handed, and a missing libstdc++ DLL is an error
    # message that looks nothing like its cause.
    if(MINGW)
        target_link_options(chatwire_exe PRIVATE
            -static -static-libgcc -static-libstdc++)
    endif()
endif()

# CHATWIRE_PLATFORM used to be interpolated here and was never set anywhere, so
# this line has been printing a trailing comma and a blank for as long as it has
# existed.  It says the two things that vary instead: which compiler, and how
# reflection had to be asked for.
if(CHATWIRE_REFLECTION_OPTIONS)
    set(CHATWIRE_REFLECTION_NOTE "reflection via ${CHATWIRE_REFLECTION_OPTIONS}")
else()
    set(CHATWIRE_REFLECTION_NOTE "reflection on by default")
endif()
message(STATUS "chatwire ${PROJECT_VERSION} - C++${CMAKE_CXX_STANDARD}, header-based, "
               "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}, ${CHATWIRE_REFLECTION_NOTE}")
