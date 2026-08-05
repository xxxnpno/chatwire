cmake_minimum_required(VERSION 3.20)

project(chatwire
    VERSION      0.3.0
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

# ---------------------------------------------------------------------------
# Platforms.
#
# chatwire builds on Windows, Linux and macOS.  What differs between them is not
# the library -- every platform difference in it sits behind one of three
# headers, chatwire/net.hpp, chatwire/module.hpp and chatwire/terminal.hpp --
# but how it GETS INTO the game, which is why the injector is a Windows target
# and the others reach the game through the loader.  See the README.
# ---------------------------------------------------------------------------
if(WIN32)
    set(CHATWIRE_PLATFORM "windows")
elseif(APPLE)
    set(CHATWIRE_PLATFORM "macos")
elseif(UNIX)
    set(CHATWIRE_PLATFORM "linux")
else()
    message(FATAL_ERROR
        "chatwire supports Windows, Linux and macOS; this is none of them.")
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
option(CHATWIRE_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)
option(CHATWIRE_BUILD_TOOLS        "Build the injector, client and mock" ON)
option(CHATWIRE_BUILD_TESTS        "Build the no-Minecraft test suite"   ON)

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
# use <print> -- it reports through chatwire/log.hpp -- so `-DCHATWIRE_BUILD_
# TOOLS=OFF -DCHATWIRE_BUILD_TESTS=OFF` genuinely drops the requirement, and
# checking anyway would refuse a configuration that would have worked.
# ---------------------------------------------------------------------------
if(CHATWIRE_BUILD_TOOLS OR CHATWIRE_BUILD_TESTS)
    include(CheckCXXSourceCompiles)

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
                "chatwire's tools and tests need a working C++23 <print>, and this "
                "compiler (${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}) "
                "has none. GCC 14+, LLVM/Clang 18+ with its own libc++, or "
                "MSVC 19.37+. On macOS the system Apple Clang is usually too old: "
                "`brew install llvm` and configure with "
                "-DCMAKE_CXX_COMPILER=$(brew --prefix llvm)/bin/clang++. "
                "Or build just the library, which needs none of it: "
                "-DCHATWIRE_BUILD_TOOLS=OFF -DCHATWIRE_BUILD_TESTS=OFF.")
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
target_link_libraries(chatwire_lib PUBLIC vmhook::vmhook Threads::Threads)
target_compile_features(chatwire_lib PUBLIC cxx_std_23)

if(WIN32)
    target_link_libraries(chatwire_lib PUBLIC ws2_32)
else()
    # dl: chatwire/module.hpp uses dladdr to find its own path on disk, the way
    # the Windows build uses GetModuleHandleEx.  Folded into libc on glibc 2.34+
    # and on macOS, still a separate library on anything older.
    target_link_libraries(chatwire_lib PUBLIC ${CMAKE_DL_LIBS})
endif()

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
if(WIN32)
    set(CHATWIRE_ENTRY src/dllmain.cpp)
else()
    set(CHATWIRE_ENTRY src/soload.cpp)
endif()

add_library(chatwire SHARED ${CHATWIRE_ENTRY})
target_link_libraries(chatwire PRIVATE chatwire::chatwire)
set_target_properties(chatwire PROPERTIES
    PREFIX ""                       # chatwire.dll/.so/.dylib, never libchatwire
    OUTPUT_NAME "chatwire")
chatwire_apply_warnings(chatwire)

if(WIN32)
    # A detached chatwire stays mapped in the game (see src/entry.hpp), which
    # locks chatwire.dll against being overwritten.  Rename the old one out of
    # the way before linking so "rebuild while Minecraft is open" just works.
    # POSIX needs nothing here: unlinking a mapped file detaches the name from
    # the inode the process is still running out of, so the link just succeeds.
    add_custom_command(TARGET chatwire PRE_LINK
        COMMAND ${CMAKE_COMMAND} -DDLL=$<TARGET_FILE:chatwire>
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/unlock_output.cmake"
        COMMENT "Moving any mapped chatwire.dll aside")
endif()

# Self-contained: loaded into a process whose library search path we do not
# control, where a missing libstdc++/libgcc fails with an error that looks
# nothing like its cause.  Only the GNU toolchains can do this -- Apple ships no
# static libc++, and -static-libstdc++ is not a clang-on-macOS option.
if(MINGW)
    target_link_options(chatwire PRIVATE -static -static-libgcc -static-libstdc++)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND NOT APPLE)
    target_link_options(chatwire PRIVATE -static-libgcc -static-libstdc++)
endif()

if(NOT WIN32)
    # Default-hidden, so the only symbols the game's dynamic linker can see are
    # the two this library exports on purpose.  It matters far more here than in
    # an ordinary shared library: chatwire is PRELOADED, which puts it first in
    # the lookup order, and a preloaded library exporting everything it happens
    # to define is how an unrelated `start` or `log` silently interposes on the
    # host program's own.
    set_target_properties(chatwire PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON)
endif()

# ---------------------------------------------------------------------------
# Tools.
# ---------------------------------------------------------------------------
if(CHATWIRE_BUILD_TOOLS)
    # Accumulated as the targets are declared, because which tools exist is
    # platform-dependent: chatwire-inject is Windows-only.
    set(CHATWIRE_TOOLS chatwire-mock chatwire-client)

    # chatwire-preload is a shell script, not a target: it sets two environment
    # variables and execs the game, and a compiled program to do that would only
    # be harder to read.  Copied next to the library because that is where it
    # looks for it -- see the script.
    if(NOT WIN32)
        add_custom_command(TARGET chatwire POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/chatwire-preload"
                    "$<TARGET_FILE_DIR:chatwire>/chatwire-preload"
            COMMAND ${CMAKE_COMMAND} -E chdir "$<TARGET_FILE_DIR:chatwire>"
                    chmod +x chatwire-preload
            COMMENT "Placing chatwire-preload beside the library")
    endif()

    # The injector talks to the OS, not to chatwire -- but it does share the
    # config format, so it needs the include path (header-only, no link).
    # Windows only, and that is a property of the platform rather than a gap in
    # the port: attaching to a process that is ALREADY RUNNING needs
    # CreateRemoteThread here, ptrace on Linux, and task_for_pid on macOS --
    # where a hardened-runtime JVM refuses it outright.  Elsewhere chatwire is
    # preloaded at launch instead; see src/soload.cpp and the README.
    if(WIN32)
        add_executable(chatwire-inject tools/injector.cpp)
        target_include_directories(chatwire-inject PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include")
        chatwire_apply_warnings(chatwire-inject)
        list(APPEND CHATWIRE_TOOLS chatwire-inject)
    endif()

    # The mock serves chatwire's protocol with synthetic chat, so a consumer can
    # be built and tested without a Minecraft client open.  It links the REAL
    # server and JSON, which is what keeps its wire format from drifting.
    add_executable(chatwire-mock tools/mock.cpp)
    target_link_libraries(chatwire-mock PRIVATE chatwire::chatwire)
    chatwire_apply_warnings(chatwire-mock)

    # The reference client speaks the protocol over a socket like any other
    # consumer would -- it links no chatwire code on purpose, and takes the
    # include path only for the socket and terminal shims.  If the reference
    # client needed privileged access to the library, the API would not be one.
    add_executable(chatwire-client tools/client.cpp)
    target_include_directories(chatwire-client PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(chatwire-client PRIVATE Threads::Threads)
    if(WIN32)
        target_link_libraries(chatwire-client PRIVATE ws2_32)
    endif()
    chatwire_apply_warnings(chatwire-client)

    # Every tool that prints needs whatever <print> turned out to require; see
    # the detection at the top of this file.
    if(CHATWIRE_PRINT_LIBRARY)
        foreach(tool ${CHATWIRE_TOOLS})
            target_link_libraries(${tool} PRIVATE ${CHATWIRE_PRINT_LIBRARY})
        endforeach()
    endif()

    if(MINGW)
        foreach(tool ${CHATWIRE_TOOLS})
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
    if(CHATWIRE_PRINT_LIBRARY)
        target_link_libraries(chatwire_test_server PRIVATE ${CHATWIRE_PRINT_LIBRARY})
    endif()
    chatwire_apply_warnings(chatwire_test_server)
    add_test(NAME server COMMAND chatwire_test_server)
endif()

message(STATUS "chatwire ${PROJECT_VERSION} - C++${CMAKE_CXX_STANDARD}, header-based, ${CHATWIRE_PLATFORM}")
