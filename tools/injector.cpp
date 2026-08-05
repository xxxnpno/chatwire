// chatwire-inject — a terminal injector for chatwire.dll.
//
// Finds the running Minecraft (a java.exe / javaw.exe), injects the DLL with the
// classic LoadLibraryW remote thread, and reports what happened.  No GUI: this
// is a tool you run and read, and a console makes it scriptable.
//
// USAGE
//   chatwire-inject                     find Minecraft, inject chatwire.dll
//   chatwire-inject --pid 1234          inject into a specific process
//   chatwire-inject --dll path.dll      inject a different DLL
//   chatwire-inject --port 9000         set CHATWIRE_PORT for the target first
//   chatwire-inject --list              list candidate processes and exit
//
// WHY LoadLibraryW AND NOT SOMETHING CLEVERER
// Manual mapping and thread hijacking exist to avoid detection.  chatwire is a
// tool its own user runs against their own game, so there is nothing to hide
// from — and LoadLibraryW is the one technique the loader itself performs, which
// means the DLL gets a real module handle, real TLS, and a real DllMain.  Every
// stealthier method gives up one of those and buys nothing here.
#include <cstdio>
#include <cstring>
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
        std::printf("  [error] %s: %lu%s%s", what, code,
                    message ? " - " : "", message ? message : "\n");
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
            std::printf("  [error] architecture mismatch: the target is %s-bit and this "
                        "injector is %s-bit\n",
                        target_is_wow64 ? "32" : "64", self_is_wow64 ? "32" : "64");
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
                        std::printf("  [error] the remote LoadLibraryW did not return in 10s\n");
                    }
                    else if (module_handle == 0u)
                    {
                        std::printf("  [error] LoadLibraryW returned NULL in the target - the "
                                    "DLL exists but could not be loaded.\n"
                                    "          Usually a missing dependency or an "
                                    "architecture mismatch.\n");
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
        std::printf(
            "chatwire-inject - inject chatwire into a running Minecraft\n"
            "\n"
            "  chatwire-inject                 find Minecraft and inject chatwire.dll\n"
            "  chatwire-inject --list          list candidate processes and exit\n"
            "  chatwire-inject --pid <n>       inject into a specific process\n"
            "  chatwire-inject --dll <path>    inject a different DLL\n"
            "  chatwire-inject --port <n>      port for chatwire to listen on\n"
            "\n"
            "The DLL defaults to chatwire.dll next to this executable.\n");
    }
}

int main(const int argc, char** const argv)
{
    std::wstring dll{ L"chatwire.dll" };
    DWORD        pid{ 0 };
    bool         list_only{ false };
    unsigned     port{ 0 };

    for (int i{ 1 }; i < argc; ++i)
    {
        const std::string arg{ argv[i] };
        const auto next{ [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : nullptr; } };

        if (arg == "--help" || arg == "-h") { usage(); return 0; }
        else if (arg == "--list") { list_only = true; }
        else if (arg == "--pid")  { if (const char* v{ next() }) { pid = std::strtoul(v, nullptr, 10); } }
        else if (arg == "--port") { if (const char* v{ next() }) { port = std::strtoul(v, nullptr, 10); } }
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
            std::printf("unknown argument: %s\n\n", arg.c_str());
            usage();
            return 2;
        }
    }

    std::printf("chatwire-inject\n\n");

    // Resolve the DLL relative to the EXECUTABLE, not the working directory:
    // the normal case is both sitting in the same folder, and a user running
    // this from elsewhere should not have to care.
    if (dll.find(L'\\') == std::wstring::npos && dll.find(L'/') == std::wstring::npos)
    {
        wchar_t self[MAX_PATH]{};
        if (::GetModuleFileNameW(nullptr, self, MAX_PATH) > 0)
        {
            std::wstring folder{ self };
            const std::size_t slash{ folder.find_last_of(L"\\/") };
            if (slash != std::wstring::npos) { dll = folder.substr(0, slash + 1) + dll; }
        }
    }
    dll = absolute_dll_path(dll);

    if (::GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        std::printf("  [error] no such DLL: %s\n", to_utf8(dll).c_str());
        return 1;
    }
    std::printf("  dll   : %s\n", to_utf8(dll).c_str());

    auto candidates{ find_java_processes() };
    annotate_with_titles(candidates);

    if (list_only || (pid == 0 && candidates.size() != 1))
    {
        if (candidates.empty())
        {
            std::printf("\n  no java.exe / javaw.exe is running.  Start Minecraft first.\n");
            return 1;
        }
        std::printf("\n  candidates:\n");
        for (const candidate& c : candidates)
        {
            std::printf("    pid %-8lu %-12s %s\n", c.pid, to_utf8(c.exe).c_str(),
                        c.window_title.empty() ? "(no window yet)"
                                               : to_utf8(c.window_title).c_str());
        }
        if (list_only) { return 0; }
        std::printf("\n  more than one candidate - pick one with --pid <n>\n");
        return 1;
    }

    if (pid == 0) { pid = candidates.front().pid; }
    std::printf("  target: pid %lu\n", pid);

    if (already_injected(pid, L"chatwire.dll"))
    {
        std::printf("\n  chatwire is already loaded in that process.\n");
        return 0;
    }

    // The port is read by chatwire from its own environment at start-up, so it
    // has to be set in the TARGET.  We cannot change another process's
    // environment, so this only works when the injector started the game -- which
    // it does not.  Report honestly rather than pretending.
    if (port != 0u)
    {
        std::printf("\n  [note] --port cannot be applied to an already-running game:\n"
                    "         chatwire reads CHATWIRE_PORT from the JVM's own environment.\n"
                    "         Set it before launching Minecraft, then inject.\n");
    }

    (void)enable_debug_privilege();

    std::printf("\n  injecting...\n");
    if (!inject(pid, dll))
    {
        std::printf("\n  injection FAILED.\n");
        return 1;
    }

    std::printf("\n  injected.  chatwire is starting inside the game;\n"
                "  it waits for Minecraft's classes, so give it a moment.\n"
                "\n  connect to  ws://127.0.0.1:%u\n",
                port != 0u ? port : 24455u);
    return 0;
}
