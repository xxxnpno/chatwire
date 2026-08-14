# 3.28 is the floor for C++20 MODULES: below it CMake does not scan sources for
# imports, so nothing is ordered, no module mapper is passed, and every consumer
# fails with "failed to read compiled module".
cmake_minimum_required(VERSION 3.28)

project(chatwire
    VERSION      0.3.0
    DESCRIPTION  "Live WebSocket API for Minecraft 1.8.9, built on vmhook"
    LANGUAGES    CXX)

# ---------------------------------------------------------------------------
# C++26, and MODULES ALL THE WAY DOWN.
#
# There is no header in this project and no .cpp either.  Every translation unit
# is a module interface unit named .ixx, every dependency between them is an
# `import`, and the standard library arrives as `import std;`.  The preprocessor
# survives in exactly one place -- the global module fragment of the eight units
# that need Win32, because <windows.h> has no module and nobody can give it one.
#
# This was tried before and abandoned, and the notes explaining why are worth
# keeping because they were TRUE OF GCC 15: vmhook segfaulted the compiler when
# instantiated inside a module, any TU that both imported a module and included
# <windows.h> failed outright, and module-attached inline statics produced
# duplicate symbols.  GCC 16.2 fixes the first two.  The third is still real and
# is handled by two things rather than wished away: `--allow-multiple-definition`
# below, and a rule in the code that state-holding functions are never `inline`
# (see chatwire::registry::detail::storage() in chatwire.feature).
#
# What modules buy, concretely:
#
#   * `import std;` replaced a 30-line #include block copied into every one of
#     the 27 units -- roughly 800 lines of preprocessor whose only job was to be
#     identical everywhere, and which had already drifted;
#   * the include ORDER rules are gone with it.  "standard library before
#     <windows.h>", "winsock2.h before windows.h", "<meta> in every unit that
#     instantiates a reflection template" were three invariants a human enforced
#     across 30 files.  Two are now nobody's problem, and the third survives
#     only inside the Win32 fragments;
#   * start() and stop() moved back into chatwire.api.  As a header this file
#     could not afford their bodies -- they reach vmhook and Winsock -- and a
#     BMI has no such cost.
#
# C++26 is not for modules, though: it is for static reflection (P2996).  Every
# JSON object this project puts on the wire is a struct whose field names ARE
# its member names, written by chatwire.json walking
# `std::meta::nonstatic_data_members_of`.  What that replaced was a positional
# `std::format("{},{},{}", field(...), field(...), field(...))` per message --
# where the count of `{}` and the count of arguments were two facts a human had
# to keep equal, and where a mistyped key was a wire format change nothing could
# catch.  See the top of chatwire/json.ixx.
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
# <print>, and the library it hides in.
#
# The tools print with std::println.  Two things vary and both have bitten a
# real build:
#
#   * the header may not exist at all (Apple Clang lags libc++ by a year or so,
#     and the system compiler on a current macOS may simply not have it);
#   * on libstdc++ the terminal path lives in a SEPARATE library, libstdc++exp,
#     and without it the link fails on __open_terminal / __write_to_terminal --
#     a link error that names symbols no source file mentions.
#
# Which of those is true is not predictable from the compiler VERSION, so it is
# compiled rather than assumed, once, here, where a missing <print> is a clear
# message at configure time instead of five hundred lines of template output.
#
# This USED to be conditional on CHATWIRE_BUILD_TOOLS, on the grounds that the
# library itself never prints.  `import std;` ended that: the std module is one
# module, it contains <print> whether a given unit calls it or not, and the
# symbols it needs have to be linkable for anything built here.  So the check is
# unconditional now, and -DCHATWIRE_BUILD_TOOLS=OFF no longer buys an escape
# from it.  The probe still #includes the header rather than importing it --
# try_compile runs before the std module target exists, and cannot use it.
# ---------------------------------------------------------------------------
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
            "chatwire needs a working C++23 <print>, and this "
            "compiler (${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}) "
            "has none. GCC 14+, LLVM/Clang 18+ with its own libc++, or "
            "MSVC 19.37+.")
    endif()
endif()
unset(CMAKE_REQUIRED_QUIET)

# ---------------------------------------------------------------------------
# `import std;` — the standard library as a module, built here.
#
# libstdc++ ships the module's SOURCE, bits/std.cc, and nothing builds it for
# you: `import std;` without a compiled std.gcm fails with "failed to read
# compiled module", which reads like a broken install and is not one.
#
# CMake does have a built-in path for this -- set CXX_MODULE_STD on a target and
# it finds libstdc++.modules.json itself -- but as of CMake 4.3 that path is
# still behind the CMAKE_EXPERIMENTAL_CXX_IMPORT_STD gate, whose value is a UUID
# that changes with every CMake release.  A build that stops working when the
# user upgrades CMake is worse than eight lines that say what they do.
#
# So: an ordinary STATIC library whose one module interface unit is the one
# libstdc++ shipped.  CMake scans it, sees `export module std;`, builds it
# before anything that imports it, and hands consumers the module mapper.
# ---------------------------------------------------------------------------
get_filename_component(CHATWIRE_CXX_BINDIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
find_file(CHATWIRE_STD_MODULE_SOURCE std.cc
    HINTS "${CHATWIRE_CXX_BINDIR}/../include/c++/${CMAKE_CXX_COMPILER_VERSION}/bits"
          "${CHATWIRE_CXX_BINDIR}/../include/c++/${CMAKE_CXX_COMPILER_VERSION}"
    DOC   "libstdc++'s source for the std module (bits/std.cc)")

if(NOT CHATWIRE_STD_MODULE_SOURCE)
    message(FATAL_ERROR
        "Could not find libstdc++'s bits/std.cc, which is the source of the "
        "`std` module. Every translation unit in chatwire says `import std;`, "
        "so there is no build without it. It ships with libstdc++ 15 and newer, "
        "at <prefix>/include/c++/<version>/bits/std.cc. Point at it with "
        "-DCHATWIRE_STD_MODULE_SOURCE=/path/to/std.cc if it lives somewhere "
        "this did not look.")
endif()

get_filename_component(CHATWIRE_STD_MODULE_DIR "${CHATWIRE_STD_MODULE_SOURCE}" DIRECTORY)

add_library(chatwire_std STATIC)
target_sources(chatwire_std PUBLIC
    FILE_SET CXX_MODULES
    BASE_DIRS "${CHATWIRE_STD_MODULE_DIR}"
    FILES     "${CHATWIRE_STD_MODULE_SOURCE}")
target_compile_features(chatwire_std PUBLIC cxx_std_26)
# GCC needs -fmodules on every TU that imports, not only on interface units.
target_compile_options(chatwire_std PUBLIC -fmodules)

# -freflection HERE, and it is load bearing rather than tidy.  std.cc exports
# <meta> only when the compiler that builds it has reflection on; without the
# flag the std module is built WITHOUT std::meta, and every unit that uses a
# reflection then fails with `'std::meta' has not been declared` -- the same
# message a missing flag on the consumer gives, pointing at the wrong file.
target_compile_options(chatwire_std PUBLIC ${CHATWIRE_REFLECTION_OPTIONS})

# GCC 16.2 emits a function-local `static` from an inline function into the
# module's object AND into every consumer's, so the linker sees each one twice.
# It happens inside libstdc++'s own std::format, so no source change of ours
# avoids it; the definitions are identical, which is what makes letting the
# linker pick one safe.  chatwire's OWN state does not rely on this -- the
# functions that hold it are deliberately not inline -- so what this pardons is
# libstdc++'s duplicates and nothing else.  Remove it when GCC stops.
#
# PUBLIC on the std module because every target here links it, which makes this
# the one place the option has to be stated.
target_link_options(chatwire_std PUBLIC "-Wl,--allow-multiple-definition")

if(CHATWIRE_PRINT_LIBRARY)
    target_link_libraries(chatwire_std PUBLIC ${CHATWIRE_PRINT_LIBRARY})
endif()


# ---------------------------------------------------------------------------
# vmhook 7.0.0 — vendored, and a module of its own.
#
# Pinned rather than fetched: chatwire depends on invocation working pure-VM,
# and on vmhook::attach_current_thread, without which a Java call would once
# again have to happen inside a detour and the pump would have to come back.  A
# floating dependency on a library this project is injected alongside is how a
# game crashes after an unrelated upgrade.
#
# The version is written here so that `vmhook::version_string` in a running
# build and this line can be compared without going and looking.  Bump both
# together when re-vendoring from C:/repos/cpp/vmhook.
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
# vmhook uses reflection itself now (display_string_of, annotations_of), so the
# flag belongs on ITS target too -- not only on chatwire_lib.
target_compile_options(vmhook PUBLIC ${CHATWIRE_REFLECTION_OPTIONS})

# vmhook does NOT link chatwire_std, and that is deliberate.  It is VENDORED --
# developed at C:/repos/cpp/vmhook and copied in here -- so it keeps upstream's
# shape: standard headers in its global module fragment rather than `import
# std;`.  Editing the copy would fork it silently, and the next re-vendor would
# overwrite the change with nothing to say so.  Nothing textual crosses a BMI,
# so a consumer that imports vmhook AND imports std is unaffected by the choice.

# ---------------------------------------------------------------------------
# The library.  Twenty-five module interface units and NOTHING ELSE.
#
# There is no source list here beside the file set, because there is no source
# that is not a module.  Five files used to sit outside it: chatwire.cpp, which
# held start()/stop(), and four *_impl.cpp that existed purely so their
# function-local statics were emitted once.  The first folded into chatwire.ixx
# once a BMI made its dependencies free; the other four folded into their own
# interfaces once the rule "state-holding functions are never inline" replaced
# the file-based workaround.  See chatwire::registry::detail::storage().
#
# CMake scans the set, works out the order and builds each unit before whatever
# imports it.  The list below is alphabetical-ish for reading, NOT dependency
# order -- there is no dependency order to keep by hand any more.
# ---------------------------------------------------------------------------
add_library(chatwire_lib STATIC)

target_sources(chatwire_lib PUBLIC
    FILE_SET CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/chatwire/src"
    FILES
        chatwire/src/chatwire/version.ixx
        chatwire/src/chatwire/stdio.ixx
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

add_library(chatwire::chatwire ALIAS chatwire_lib)

# No target_include_directories.  Nothing includes anything -- a consumer says
# `import chatwire.api;` and CMake hands the compiler a module mapper, so there
# is no path for a header search to be wrong about.
target_link_libraries(chatwire_lib PUBLIC chatwire_std vmhook::vmhook Threads::Threads)
target_compile_features(chatwire_lib PUBLIC cxx_std_26)

# PUBLIC, and that is not tidiness: chatwire.json is part of the public surface
# and its writer is a template, so the reflection in it is instantiated in the
# CONSUMER's translation unit, not in this library's.  A consumer built without
# the flag gets `'std::meta' has not been declared` out of a module it only
# meant to import.
target_compile_options(chatwire_lib PUBLIC ${CHATWIRE_REFLECTION_OPTIONS})

target_link_libraries(chatwire_lib PUBLIC ws2_32 bcrypt)

# ---------------------------------------------------------------------------
# Warnings.
#
# vmhook is a 27k-line vendored module; its diagnostics are not chatwire's to
# fix, and -Werror on somebody else's code only ever produces noise.  So the
# warning set applies to chatwire's own targets, and neither vmhook nor the std
# module -- which is libstdc++'s file, not ours either -- gets it.
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
#
# dllmain.ixx is a module interface unit like everything else, even though
# nothing imports it: it exists to hold DllMain and the two exported functions.
# It has to be in a CXX_MODULES file set all the same -- a source that says
# `export module` and is not in one makes CMake stop with "provides the
# `chatwire.dllmain` module but it is not found in a `FILE_SET`".
#
# What that unit does NOT get to do is let the module attach its entry points;
# see the linkage note at the top of it.
# ---------------------------------------------------------------------------
add_library(chatwire SHARED)
target_sources(chatwire PRIVATE
    FILE_SET CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/chatwire/src"
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/chatwire/src/dllmain.ixx")
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
    # The target is chatwire_exe and the FILE is chatwire.exe: two CMake
    # targets cannot share a name, but chatwire.dll and chatwire.exe are
    # different files and may.
    #
    # injector.ixx is a module unit and needs a file set for the same reason
    # dllmain.ixx does.  It is also where `main` lives, which is the one thing a
    # module may not own -- see the note above main() in that file.
    add_executable(chatwire_exe)
    target_sources(chatwire_exe PRIVATE
        FILE_SET CXX_MODULES
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/chatwire/src"
        FILES "${CMAKE_CURRENT_SOURCE_DIR}/chatwire/src/injector.ixx")
    set_target_properties(chatwire_exe PROPERTIES OUTPUT_NAME "chatwire")
    # It imports chatwire.config, whose reader and writer are both generated
    # from the settings struct, so it instantiates reflection in its own TU.
    # Stated here as well as on the library because a compile OPTION is what the
    # instantiation needs and the link alone would not carry it.
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

    # The injector imports chatwire.config to write the handover file, so it
    # needs the target that OWNS that module.  <print>'s library rides in on the
    # same edge, from chatwire_std; see the top of this file.
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
message(STATUS "chatwire ${PROJECT_VERSION} - C++${CMAKE_CXX_STANDARD}, named modules + import std, "
               "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}, ${CHATWIRE_REFLECTION_NOTE}")
