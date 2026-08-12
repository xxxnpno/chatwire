// chatwire.exe — chatwire, and the tool that puts it into the game, in one file.
//
// Finds the running Minecraft (a java.exe / javaw.exe), injects the library with
// the classic LoadLibraryW remote thread, and reports what happened.  No GUI:
// this is a tool you run and read, and a console makes it scriptable.
//
// USAGE
//   chatwire                     find Minecraft and inject
//   chatwire --pid 1234          inject into a specific process
//   chatwire --port 9000         listen on a different port
//   chatwire --console           also open a console showing live chat
//   chatwire --list              list candidate processes and exit
//   chatwire --dll path.dll      inject a DLL from disk instead of the built-in
//
// ===========================================================================
// THE DLL IS INSIDE THIS EXECUTABLE
// ===========================================================================
// chatwire.exe carries chatwire.dll as a Windows resource, so this is ONE FILE
// you can hand to somebody.  There is no folder to keep together and, more to
// the point, no way to end up running an injector from one build against a DLL
// from another -- which produced exactly the kind of bug report that wastes an
// evening: an injection that succeeds and a library that behaves like an older
// version, because it was one.
//
// Injecting still needs a PATH, because LoadLibraryW takes one -- the target
// process reads the file itself, and it cannot read our address space.  So the
// resource is written to a temporary file first.  See extract_library() for
// where it goes and why the name is what it is.
//
// --dll overrides all of that and injects a file from disk, which is what you
// want while developing the library itself.
//
// WHY LoadLibraryW AND NOT SOMETHING CLEVERER
// Manual mapping and thread hijacking exist to avoid detection.  chatwire is a
// tool its own user runs against their own game, so there is nothing to hide
// from — and LoadLibraryW is the one technique the loader itself performs, which
// means the DLL gets a real module handle, real TLS, and a real DllMain.  Every
// stealthier method gives up one of those and buys nothing here.
#include "chatwire/config.hpp"

#include <print>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include <windows.h>
#include <tlhelp32.h>

namespace
{
    struct candidate
    {
        DWORD       pid{ 0 };
        std::wstring exe{};
        std::wstring window_title{};
    };

    auto print_error(const char* const what) -> void
    {
        const DWORD code{ ::GetLastError() };
        char* message{ nullptr };
        ::FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
                             | FORMAT_MESSAGE_IGNORE_INSERTS,
                         nullptr, code, 0, reinterpret_cast<LPSTR>(&message), 0, nullptr);
        std::print("  [error] {}: {}{}{}", what, code, message ? " - " : "", message ? message : "\n");
        if (message) { ::LocalFree(message); }
    }

    /*
        @brief Every java.exe / javaw.exe currently running.
        @details
        Both names, because a launcher may use either: javaw is the windowless
        one Minecraft normally runs under, but a dev environment or a wrapper
        script often uses java.exe so the console stays visible.
    */
    auto find_java_processes() -> std::vector<candidate>
    {
        std::vector<candidate> found;

        const HANDLE snapshot{ ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0) };
        if (snapshot == INVALID_HANDLE_VALUE) { return found; }

        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (::Process32FirstW(snapshot, &entry))
        {
            do
            {
                const std::wstring name{ entry.szExeFile };
                if (name == L"javaw.exe" || name == L"java.exe")
                {
                    found.push_back(candidate{ entry.th32ProcessID, name, {} });
                }
            } while (::Process32NextW(snapshot, &entry));
        }
        ::CloseHandle(snapshot);
        return found;
    }

    /* Window titles make the list readable: several JVMs may be running, and
       "Minecraft 1.8.9" is how a human tells which is the game. */
    auto BOOL_CALLBACK_title(const HWND window, const LPARAM param) -> BOOL
    {
        auto* const list{ reinterpret_cast<std::vector<candidate>*>(param) };
        DWORD owner{ 0 };
        ::GetWindowThreadProcessId(window, &owner);

        for (candidate& c : *list)
        {
            if (c.pid != owner || !c.window_title.empty()) { continue; }
            wchar_t title[256]{};
            if (::GetWindowTextW(window, title, 255) > 0 && ::IsWindowVisible(window))
            {
                c.window_title = title;
            }
        }
        return TRUE;
    }

    auto annotate_with_titles(std::vector<candidate>& list) -> void
    {
        ::EnumWindows(&BOOL_CALLBACK_title, reinterpret_cast<LPARAM>(&list));
    }

    /*
        @brief Enables SeDebugPrivilege so we can open a process we did not start.
        @details
        Not always required — a process running as the same user is usually
        openable without it — but a Minecraft launched by a launcher running
        elevated is not.  Failing here is not fatal; the OpenProcess below will
        report the real problem if there is one.
    */
    auto enable_debug_privilege() noexcept -> bool
    {
        HANDLE token{ nullptr };
        if (!::OpenProcessToken(::GetCurrentProcess(),
                                TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        {
            return false;
        }

        LUID luid{};
        bool ok{ false };
        if (::LookupPrivilegeValueW(nullptr, L"SeDebugPrivilege", &luid))
        {
            TOKEN_PRIVILEGES privileges{};
            privileges.PrivilegeCount           = 1;
            privileges.Privileges[0].Luid       = luid;
            privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            ok = ::AdjustTokenPrivileges(token, FALSE, &privileges, sizeof(privileges),
                                         nullptr, nullptr) != 0
                 && ::GetLastError() == ERROR_SUCCESS;
        }
        ::CloseHandle(token);
        return ok;
    }

    /* @brief True if `pid` already has our DLL loaded. */
    auto already_injected(const DWORD pid, const std::wstring& dll_name) -> bool
    {
        const HANDLE snapshot{ ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE
                                                          | TH32CS_SNAPMODULE32, pid) };
        if (snapshot == INVALID_HANDLE_VALUE) { return false; }

        bool found{ false };
        MODULEENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (::Module32FirstW(snapshot, &entry))
        {
            do
            {
                if (_wcsicmp(entry.szModule, dll_name.c_str()) == 0) { found = true; break; }
            } while (::Module32NextW(snapshot, &entry));
        }
        ::CloseHandle(snapshot);
        return found;
    }

    /*
        @brief Asks an already-loaded chatwire to start again.
        @details
        A detach leaves the module mapped, and LoadLibrary on an already-loaded
        module does not re-run DllMain, so re-injecting has to wake the copy that
        is already there.

        It is done with a named event rather than by calling into the target,
        because calling means knowing an address, and the only cheap source of one
        is the DLL on disk -- which stops matching the mapped module the moment it
        is rebuilt.  An event carries no addresses, so it cannot be stale.

        @return true when the signal was delivered.  A false here usually means
                the loaded copy predates this mechanism.
    */
    auto signal_restart(const DWORD pid) -> bool
    {
        // Spelled exactly as dllmain.cpp's restart_event_name() spells it; the
        // two are separate binaries, so the format string is the contract.
        const std::string name{ std::format("Local\\chatwire.restart.{}", pid) };
        const HANDLE signal{ ::OpenEventA(EVENT_MODIFY_STATE, FALSE, name.c_str()) };
        if (signal == nullptr) { return false; }
        const bool ok{ ::SetEvent(signal) != 0 };
        ::CloseHandle(signal);
        return ok;
    }

    /*
        @brief Injects `dll_path` into `pid` via a LoadLibraryW remote thread.
        @details
        The DLL path is written into the target's address space, then a thread is
        started there whose entry point IS LoadLibraryW and whose argument is that
        path.  kernel32 is loaded at the same address in every process on a given
        boot, so our LoadLibraryW address is also the target's.

        The remote thread is WAITED ON, and its exit code is LoadLibraryW's return
        value: the module handle on success, 0 on failure.  Not waiting is how an
        injector reports success for a DLL that never loaded.
    */
    auto inject(const DWORD pid, const std::wstring& dll_path) -> bool
    {
        const HANDLE process{ ::OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION
                                                | PROCESS_VM_OPERATION | PROCESS_VM_WRITE
                                                | PROCESS_VM_READ,
                                            FALSE, pid) };
        if (process == nullptr)
        {
            print_error("OpenProcess (try running as administrator)");
            return false;
        }

        // 64-bit injector, 64-bit target.  Injecting across bitness silently
        // produces a thread that cannot run, so refuse rather than "succeed".
        BOOL target_is_wow64{ FALSE };
        BOOL self_is_wow64{ FALSE };
        (void)::IsWow64Process(process, &target_is_wow64);
        (void)::IsWow64Process(::GetCurrentProcess(), &self_is_wow64);
        if (target_is_wow64 != self_is_wow64)
        {
            std::println("  [error] architecture mismatch: the target is {}-bit and this "
                        "injector is {}-bit", target_is_wow64 ? "32" : "64", self_is_wow64 ? "32" : "64");
            ::CloseHandle(process);
            return false;
        }

        const SIZE_T bytes{ (dll_path.size() + 1u) * sizeof(wchar_t) };
        void* const remote{ ::VirtualAllocEx(process, nullptr, bytes,
                                             MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE) };
        if (remote == nullptr)
        {
            print_error("VirtualAllocEx");
            ::CloseHandle(process);
            return false;
        }

        bool ok{ false };
        if (!::WriteProcessMemory(process, remote, dll_path.c_str(), bytes, nullptr))
        {
            print_error("WriteProcessMemory");
        }
        else
        {
            const HMODULE kernel32{ ::GetModuleHandleW(L"kernel32.dll") };
            auto* const loader{ reinterpret_cast<LPTHREAD_START_ROUTINE>(
                reinterpret_cast<void*>(::GetProcAddress(kernel32, "LoadLibraryW"))) };

            if (loader == nullptr)
            {
                print_error("GetProcAddress(LoadLibraryW)");
            }
            else
            {
                const HANDLE thread{ ::CreateRemoteThread(process, nullptr, 0, loader,
                                                          remote, 0, nullptr) };
                if (thread == nullptr)
                {
                    print_error("CreateRemoteThread");
                }
                else
                {
                    // 10 s is generous: LoadLibraryW returns as soon as DllMain
                    // does, and chatwire's DllMain only spawns a thread.
                    const DWORD waited{ ::WaitForSingleObject(thread, 10000) };
                    DWORD       module_handle{ 0 };
                    (void)::GetExitCodeThread(thread, &module_handle);
                    ::CloseHandle(thread);

                    if (waited == WAIT_TIMEOUT)
                    {
                        std::println("  [error] the remote LoadLibraryW did not return in 10s");
                    }
                    else if (module_handle == 0u)
                    {
                        std::println("  [error] LoadLibraryW returned NULL in the target - the "
                                    "DLL exists but could not be loaded.\n"
                                    "          Usually a missing dependency or an "
                                    "architecture mismatch.");
                    }
                    else
                    {
                        ok = true;
                    }
                }
            }
        }

        // Free either way: on failure this is a leak in someone's game, and on
        // success LoadLibraryW has already copied the path it needed.
        (void)::VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        ::CloseHandle(process);
        return ok;
    }

    /*
        @brief The library bytes carried in this executable, or empty.
        @details
        Empty when this was built without the resource, which is a supported
        configuration rather than a broken one -- see the fallback in main().
    */
    auto embedded_library() -> std::span<const unsigned char>
    {
        // MAKEINTRESOURCEW(10) rather than RT_RCDATA: the RT_* macros follow
        // the UNICODE define, and this project does not set it, so RT_RCDATA is
        // the ANSI spelling and will not go into the W function.
        const HRSRC found{ ::FindResourceW(nullptr, L"CHATWIRE_DLL",
                                           MAKEINTRESOURCEW(10)) };
        if (found == nullptr) { return {}; }

        const DWORD size{ ::SizeofResource(nullptr, found) };
        const HGLOBAL loaded{ ::LoadResource(nullptr, found) };
        if (size == 0u || loaded == nullptr) { return {}; }

        // LockResource does not lock anything and there is nothing to free: on
        // modern Windows a resource is simply part of the mapped image, so this
        // is a pointer into our own read-only pages that stays valid for the
        // lifetime of the process.
        const auto* const bytes{ static_cast<const unsigned char*>(::LockResource(loaded)) };
        if (bytes == nullptr) { return {}; }
        return { bytes, size };
    }

    /*
        @brief A 64-bit FNV-1a hash of the embedded library.
        @details
        Used to NAME the directory the library is unpacked into, which turns a
        nasty little problem into a non-problem.

        The unpacked file cannot simply be overwritten on each run: a detached
        chatwire stays mapped in the game (deliberately -- see the README), so
        the file is locked for as long as that game is open.  Overwriting would
        fail, and reusing whatever happens to be there would silently inject a
        DIFFERENT BUILD than the one inside this executable.

        Naming the directory after the contents makes those two cases the same
        case: a path that already exists holds exactly these bytes, so a failed
        write is not an error, it is proof the work was already done.  A new
        build hashes differently and unpacks somewhere else, and never has to
        touch a file the game is holding.

        Not a cryptographic hash and does not need to be: it is protecting
        against build A being mistaken for build B, not against somebody
        constructing a collision on their own machine to attack themselves.
    */
    auto content_hash(const std::span<const unsigned char> bytes) -> std::uint64_t
    {
        std::uint64_t hash{ 0xcbf29ce484222325ull };
        for (const unsigned char b : bytes)
        {
            hash ^= b;
            hash *= 0x100000001b3ull;
        }
        return hash;
    }

    /*
        @brief Writes the embedded library to a temporary file and returns it.
        @details
        The file is always called chatwire.dll, and the HASH goes in the
        DIRECTORY name rather than the file name.  That is not cosmetic: the
        loaded module takes the name of the file, and already_injected() below
        recognises an existing chatwire by that name.  Putting the hash in the
        file name would mean two different builds inject two modules with
        different names, neither of which recognises the other, and the game
        quietly ends up with two chatwires in it.

        @return the full path, or empty on failure.
    */
    auto extract_library() -> std::wstring
    {
        const auto bytes{ embedded_library() };
        if (bytes.empty()) { return {}; }

        wchar_t temp[MAX_PATH]{};
        if (::GetTempPathW(MAX_PATH, temp) == 0u) { return {}; }

        // `{:016x}` is `%016llx` with the buffer and the length argument gone --
        // and with them the only way this could have truncated.  swprintf's 32
        // wide characters were never too few for sixteen hex digits, but that is
        // a fact a reader had to check rather than one the code stated.
        //
        // The `unsigned long long` cast went with them.  It was there to satisfy
        // `%llx`, which is a promise about a varargs argument's type that only
        // the programmer can make; std::format takes the argument's own type.
        const std::wstring stamp{ std::format(L"{:016x}", content_hash(bytes)) };

        // std::format has a wide overload, so a path built from wchar_t buffers
        // needs no widening dance: the literal is L"" and the result is a
        // std::wstring.  GetTempPathW's buffer is handed over as a pointer
        // because the formatter for a character array is the const one.
        const std::wstring folder{ std::format(L"{}chatwire\\{}",
                                               static_cast<const wchar_t*>(temp), stamp) };
        // Both levels, and neither failure is fatal on its own: ALREADY_EXISTS
        // is the normal case on the second run.
        (void)::CreateDirectoryW(
            std::format(L"{}chatwire", static_cast<const wchar_t*>(temp)).c_str(), nullptr);
        (void)::CreateDirectoryW(folder.c_str(), nullptr);

        const std::wstring path{ folder + L"\\chatwire.dll" };

        // CREATE_NEW, not CREATE_ALWAYS: if it is already there it is already
        // right (the directory is named after these very bytes), and it may be
        // mapped into a running game, where overwriting would fail anyway.
        const HANDLE file{ ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                         CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr) };
        if (file == INVALID_HANDLE_VALUE)
        {
            if (::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
            {
                return path;                       // already unpacked; identical
            }
            print_error("could not unpack the library");
            return {};
        }

        DWORD written{ 0 };
        const bool ok{ ::WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()),
                                   &written, nullptr) != 0
                       && written == bytes.size() };
        ::CloseHandle(file);

        if (!ok)
        {
            print_error("could not write the unpacked library");
            // A half-written DLL is worse than none: it would load, or fail in
            // a way that looks like a bug in chatwire rather than in the disk.
            (void)::DeleteFileW(path.c_str());
            return {};
        }
        return path;
    }

    auto to_utf8(const std::wstring& text) -> std::string
    {
        if (text.empty()) { return {}; }
        const int n{ ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                           static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr) };
        std::string out(static_cast<std::size_t>(n), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                              out.data(), n, nullptr, nullptr);
        return out;
    }

    auto absolute_dll_path(const std::wstring& given) -> std::wstring
    {
        wchar_t full[MAX_PATH]{};
        if (::GetFullPathNameW(given.c_str(), MAX_PATH, full, nullptr) == 0)
        {
            return given;
        }
        return full;
    }

    auto usage() -> void
    {
        std::println("chatwire - put chatwire into a running Minecraft 1.8.9\n"
            "\n"
            "  chatwire                 find Minecraft and inject\n"
            "  chatwire --list          list candidate processes and exit\n"
            "  chatwire --pid <n>       inject into a specific process\n"
            "  chatwire --port <n>      port for chatwire to listen on\n"
            "  chatwire --bind <addr>   listen on <addr> rather than 127.0.0.1;\n"
            "                           requires --token\n"
            "  chatwire --token <s>     shared secret.  A client proves it knows\n"
            "                           this with HMAC-SHA256 over a nonce, so the\n"
            "                           secret itself is never sent.  NOT\n"
            "                           encryption -- tunnel it across a network\n"
            "                           you do not control\n"
            "  chatwire --console       ALSO open a console showing chat\n"
            "  chatwire --verbose       show chatwire's start-up trace\n"
            "  chatwire --dll <path>    inject a DLL from disk instead of the\n"
            "                           one built into this executable\n"
            "\n"
            "The library is carried inside this file, so this is the only file\n"
            "you need.  Then connect a WebSocket to ws://127.0.0.1:24455 and\n"
            "drive the game from any language; the README has an example of\n"
            "every command, and mcp/ exposes it to an AI.");
    }
}

int main(const int argc, char** const argv)
{
    // Empty means "use the copy built into this executable".  --dll fills it in.
    std::wstring dll{};
    DWORD        pid{ 0 };
    bool         list_only{ false };
    unsigned     port{ 0 };
    // No console by DEFAULT.  chatwire's interface is the websocket; the
    // console is a convenience for watching chat, and one that has to share the
    // game's window when the game has one.  Opt in with --console.
    bool         console{ false };
    bool         verbose{ false };
    std::string  bind_address{};
    std::string  token{};

    for (int i{ 1 }; i < argc; ++i)
    {
        const std::string arg{ argv[i] };
        const auto next{ [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : nullptr; } };

        if (arg == "--help" || arg == "-h") { usage(); return 0; }
        else if (arg == "--list") { list_only = true; }
        else if (arg == "--console") { console = true; }
        else if (arg == "--background") { console = false; }   // kept: it is what it says
        else if (arg == "--verbose") { verbose = true; }
        else if (arg == "--pid")  { if (const char* v{ next() }) { pid = std::strtoul(v, nullptr, 10); } }
        else if (arg == "--port") { if (const char* v{ next() }) { port = std::strtoul(v, nullptr, 10); } }
        else if (arg == "--bind")  { if (const char* v{ next() }) { bind_address = v; } }
        else if (arg == "--token") { if (const char* v{ next() }) { token = v; } }
        else if (arg == "--dll")
        {
            if (const char* v{ next() })
            {
                const int n{ ::MultiByteToWideChar(CP_UTF8, 0, v, -1, nullptr, 0) };
                dll.assign(static_cast<std::size_t>(n - 1), L'\0');
                ::MultiByteToWideChar(CP_UTF8, 0, v, -1, dll.data(), n);
            }
        }
        else
        {
            std::println("unknown argument: {}\n", arg);
            usage();
            return 2;
        }
    }

    std::println("chatwire\n");

    // Where the library comes from, in order of how much the user asked for it:
    //
    //   1. --dll <path>          exactly that file, for developing the library
    //   2. the built-in copy      unpacked to a temp file; the normal case
    //   3. chatwire.dll beside    the fallback for a build without the resource
    //      this executable
    //
    // (3) is not dead code: -DCHATWIRE_EMBED_DLL=OFF is a supported way to
    // build, and the two-file layout it produces is how this tool worked before
    // the library moved inside it.
    bool built_in{ false };
    if (dll.empty())
    {
        dll = extract_library();
        built_in = !dll.empty();

        if (dll.empty())
        {
            wchar_t self[MAX_PATH]{};
            if (::GetModuleFileNameW(nullptr, self, MAX_PATH) > 0)
            {
                std::wstring folder{ self };
                const std::size_t slash{ folder.find_last_of(L"\\/") };
                if (slash != std::wstring::npos)
                {
                    dll = folder.substr(0, slash + 1) + L"chatwire.dll";
                }
            }
        }
    }
    else if (dll.find(L'\\') == std::wstring::npos && dll.find(L'/') == std::wstring::npos)
    {
        // A bare name given to --dll is resolved against the EXECUTABLE, not the
        // working directory: a user running this from elsewhere should not have
        // to care where they happen to be standing.
        wchar_t self[MAX_PATH]{};
        if (::GetModuleFileNameW(nullptr, self, MAX_PATH) > 0)
        {
            std::wstring folder{ self };
            const std::size_t slash{ folder.find_last_of(L"\\/") };
            if (slash != std::wstring::npos) { dll = folder.substr(0, slash + 1) + dll; }
        }
    }

    if (dll.empty())
    {
        std::println("  [error] this build carries no library and none was given.\n"
                    "          Pass --dll <path>.");
        return 1;
    }
    dll = absolute_dll_path(dll);

    if (::GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        std::println("  [error] no such DLL: {}", to_utf8(dll));
        return 1;
    }
    std::println("  library: {}{}", to_utf8(dll), built_in ? "   (built in)" : "");

    auto candidates{ find_java_processes() };
    annotate_with_titles(candidates);

    if (list_only || (pid == 0 && candidates.size() != 1))
    {
        if (candidates.empty())
        {
            std::println("\n  no java.exe / javaw.exe is running.  Start Minecraft first.");
            return 1;
        }
        std::println("\n  candidates:");
        for (const candidate& c : candidates)
        {
            std::println("    pid {} {} {}", c.pid, to_utf8(c.exe), c.window_title.empty() ? "(no window yet)"
                                               : to_utf8(c.window_title));
        }
        if (list_only) { return 0; }
        std::println("\n  more than one candidate - pick one with --pid <n>");
        return 1;
    }

    if (pid == 0) { pid = candidates.front().pid; }
    std::println("  target : pid {}", pid);

    if (already_injected(pid, L"chatwire.dll"))
    {
        // Loaded, but not necessarily RUNNING: a detach stops chatwire and
        // leaves the module mapped.  So a second injection is a CALL, not a
        // load -- into the restart entry point the DLL exports for this.
        std::println("\n  chatwire is already loaded; restarting it in place.");
        if (signal_restart(pid))
        {
            std::println("  restarted.  connect to  ws://127.0.0.1:{}", port);
            return 0;
        }
        std::println("  could not restart it (already running?).");
        return 1;
    }

    // Hand the settings over through a file beside the DLL.  The injector cannot
    // change an already-running process's environment -- that is fixed when the
    // process is created -- so a flag has to reach chatwire some other way.  The
    // DLL reads this during start-up and DELETES it, so nothing is left behind
    // to apply to a later injection.
    //
    // NAMED AFTER THE TARGET PID, because the directory it goes in is named
    // after the library's hash and is therefore shared by every game on the
    // machine running this build.  One file there is one file two concurrent
    // injections fight over, each reading the other's port.  See
    // config::file_name_for.
    {
        chatwire::config::settings settings{};
        settings.port    = static_cast<std::uint16_t>(port);
        settings.console = console;
        settings.verbose = verbose;
        settings.bind    = bind_address;
        settings.token   = token;

        const std::string cfg{
            to_utf8(dll.substr(0, dll.find_last_of(L"\\/") + 1u))
            + chatwire::config::file_name_for(static_cast<std::uint32_t>(pid)) };
        if (!chatwire::config::write(cfg, settings))
        {
            std::println("\n  [warn] could not write {};\n"
                        "         chatwire will start with its defaults.", cfg);
        }
        else
        {
            std::println("  port   : {}{}{}", port != 0u ? port : 24455u, console ? "   (console)" : "   (no console)", verbose ? "   (verbose)" : "");
        }
    }

    if (!bind_address.empty() && bind_address != "127.0.0.1" && bind_address != "localhost"
        && token.empty())
    {
        std::println("\n  --bind {} needs --token.\n"
                     "  This socket can send chat as the player and read everything\n"
                     "  they see; on anything but loopback it must be authenticated.",
                     bind_address);
        return 1;
    }

    (void)enable_debug_privilege();

    std::println("\n  injecting...");
    if (!inject(pid, dll))
    {
        std::println("\n  injection FAILED.");
        return 1;
    }

    std::println("\n  injected.  chatwire is starting inside the game;\n"
                "  it waits for Minecraft's classes, so give it a moment.\n"
                "\n  connect to  ws://127.0.0.1:{}", port != 0u ? port : 24455u);
    return 0;
}
