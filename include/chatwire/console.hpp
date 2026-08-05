// chatwire/console.hpp — the console attached to the injected game.
//
// ===========================================================================
// WHY THE X IS DISABLED, AND WHAT TO DO INSTEAD
// ===========================================================================
// Closing a console window sends CTRL_CLOSE_EVENT to every process attached to
// it, and Windows then TERMINATES those processes -- roughly five seconds later,
// whatever the handler returns.  That is not something a handler can decline;
// it is the documented behaviour of closing a console.
//
// The process attached to this console is Minecraft.  So a console with a
// working close button is a button that kills the game, and no amount of
// cleanup in a CTRL_CLOSE_EVENT handler changes that -- the cleanup runs, and
// then the game dies anyway.
//
// chatwire therefore REMOVES the close button (which also disables Alt+F4, since
// both route through SC_CLOSE), and gives you a command instead:
//
//     detach      unload chatwire cleanly and close this window
//
// That does what closing the window was meant to do -- stop chatwire, put the
// hooks back, let go of the game -- and the game keeps running.
//
// The CTRL_CLOSE_EVENT handler is still installed, as a safety net for the paths
// that can still reach it (Task Manager closing the window, a console host
// crash).  It performs the same clean shutdown, so if the game is going to die
// it at least dies with its hooks removed rather than with detours pointing into
// a DLL that is being unloaded.
#pragma once

#include "chatwire/common.hpp"
#include "chatwire/ansi.hpp"
#include "chatwire/log.hpp"

#include <windows.h>

namespace chatwire::console
{
    namespace detail
    {
        /* What `detach` should call.  Set by attach(); a plain function pointer
           so there is nothing to destroy at exit. */
        inline std::atomic<void (*)()> g_on_detach{ nullptr };
        inline std::atomic<bool>       g_attached{ false };
        inline std::atomic<bool>       g_stopping{ false };

        inline auto do_detach() noexcept -> void
        {
            // exchange, not load+store: the console command thread and the
            // CTRL handler can both arrive here, and unloading twice would be
            // a double free of everything chatwire owns.
            if (g_stopping.exchange(true, std::memory_order_acq_rel)) { return; }
            if (const auto callback{ g_on_detach.load(std::memory_order_acquire) })
            {
                callback();
            }
        }

        inline auto WINAPI ctrl_handler(const DWORD event) -> BOOL
        {
            switch (event)
            {
            case CTRL_C_EVENT:
            case CTRL_BREAK_EVENT:
            case CTRL_CLOSE_EVENT:
            case CTRL_LOGOFF_EVENT:
            case CTRL_SHUTDOWN_EVENT:
                chatwire::log::raw("\x1b[93m  console closing - detaching chatwire...\x1b[0m");
                do_detach();
                return TRUE;
            default:
                return FALSE;
            }
        }

        /*
            @brief Reads console commands until `detach` or the console goes away.
            @details
            Its own thread: it blocks on stdin, and the game must not.  Exits when
            detach() runs, when stdin closes, or when the window is gone.
        */
        inline auto command_loop() noexcept -> void
        {
            std::array<char, 256> line{};
            while (g_attached.load(std::memory_order_acquire))
            {
                if (std::fgets(line.data(), static_cast<int>(line.size()), stdin) == nullptr)
                {
                    break;                       // stdin closed
                }
                std::string command{ line.data() };
                while (!command.empty()
                       && (command.back() == '\n' || command.back() == '\r'
                           || command.back() == ' '))
                {
                    command.pop_back();
                }

                if (command.empty()) { continue; }
                if (command == "detach" || command == "quit" || command == "exit")
                {
                    do_detach();
                    return;
                }
                if (command == "help" || command == "?")
                {
                    chatwire::log::raw(
                        "\n  \x1b[97mcommands\x1b[0m\n"
                        "    detach    unload chatwire and close this window\n"
                        "    verbose   show the start-up trace as well as chat\n"
                        "    quiet     chat and problems only (the default)\n"
                        "    help      this\n");
                    continue;
                }
                if (command == "verbose")
                {
                    chatwire::log::set_level(chatwire::log::level::info);
                    chatwire::log::raw("  \x1b[90mverbose logging on\x1b[0m");
                    continue;
                }
                if (command == "quiet")
                {
                    chatwire::log::set_level(chatwire::log::level::warning);
                    chatwire::log::raw("  \x1b[90mverbose logging off\x1b[0m");
                    continue;
                }
                chatwire::log::raw("  \x1b[90munknown command; try 'help'\x1b[0m");
            }
        }
    }

    /*
        @brief Creates the console, if the process has none.
        @details
        A game launched through javaw.exe has no console, so one has to be
        allocated before anything can be printed.  A game launched through
        java.exe already has one, and hijacking it would tangle chatwire's output
        with the game's own -- so in that case we attach to what is there.

        @param on_detach  Called when the user types `detach` (or the console is
                          closed by something we cannot refuse).  Must perform
                          the full chatwire shutdown.
        @return false when no console could be obtained; chatwire runs on
                without one, since the WebSocket API is the real interface.
    */
    inline auto attach(void (*const on_detach)()) noexcept -> bool
    {
        if (detail::g_attached.load(std::memory_order_acquire)) { return true; }

        const bool had_console{ ::GetConsoleWindow() != nullptr };
        if (!had_console && !::AllocConsole()) { return false; }

        FILE* stream{ nullptr };
        (void)::freopen_s(&stream, "CONOUT$", "w", stdout);
        (void)::freopen_s(&stream, "CONOUT$", "w", stderr);
        (void)::freopen_s(&stream, "CONIN$",  "r", stdin);

        // ANSI escapes, or the colour codes print as literal garbage.
        if (const HANDLE out{ ::GetStdHandle(STD_OUTPUT_HANDLE) }; out != INVALID_HANDLE_VALUE)
        {
            DWORD mode{ 0 };
            if (::GetConsoleMode(out, &mode))
            {
                (void)::SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
            }
        }
        // The game sends UTF-8; without this every § and any non-ASCII player
        // name prints as mojibake.
        (void)::SetConsoleOutputCP(CP_UTF8);
        (void)::SetConsoleTitleA("chatwire");

        detail::g_on_detach.store(on_detach, std::memory_order_release);
        (void)::SetConsoleCtrlHandler(&detail::ctrl_handler, TRUE);

        // Remove the close button.  See the header comment: closing a console
        // terminates the processes attached to it, and the process attached to
        // this one is the game.
        if (const HWND window{ ::GetConsoleWindow() }; window != nullptr)
        {
            if (HMENU menu{ ::GetSystemMenu(window, FALSE) }; menu != nullptr)
            {
                (void)::DeleteMenu(menu, SC_CLOSE, MF_BYCOMMAND);
                (void)::DrawMenuBar(window);
            }
        }

        detail::g_attached.store(true, std::memory_order_release);

        try
        {
            std::thread{ &detail::command_loop }.detach();
        }
        catch (...)
        {
            // No command thread: the console still shows chat, and `detach` is
            // simply unavailable.  Not worth failing start-up over.
            chatwire::log::warn("console: could not start the command reader; "
                                "'detach' will not work");
        }
        return true;
    }

    /* @brief The banner.  Uses raw(), because it is not a diagnostic. */
    inline auto banner(const std::string_view version, const std::uint16_t port,
                       const std::string_view mapping) noexcept -> void
    {
        try
        {
            chatwire::log::raw("");
            chatwire::log::raw(std::format(
                "  \x1b[92mchatwire {}\x1b[0m  \x1b[90m|\x1b[0m  ws://127.0.0.1:{}"
                "  \x1b[90m|\x1b[0m  {}", version, port, mapping));
            chatwire::log::raw(
                "  \x1b[90mchat appears below.  type 'help' for commands, "
                "'detach' to unload.\x1b[0m");
            chatwire::log::raw(
                "  \x1b[90m(the close button is disabled on purpose - closing a "
                "console kills the game)\x1b[0m");
            chatwire::log::raw("");
        }
        catch (...) { }
    }

    /* @brief Prints one chat line, colours and all. */
    inline auto chat_line(const std::string_view formatted) noexcept -> void
    {
        try { chatwire::log::raw(chatwire::ansi::render(formatted)); }
        catch (...) { }
    }

    /* @brief A chatwire event, visually distinct from game chat. */
    inline auto event(const std::string_view text) noexcept -> void
    {
        try { chatwire::log::raw(std::format("  \x1b[90m* {}\x1b[0m", text)); }
        catch (...) { }
    }

    /*
        @brief Releases the console.  Called from the detach path.
        @details
        Restores the close button first: the window is about to belong to
        nothing, and leaving it unclosable would strand it on the desktop.
    */
    inline auto release() noexcept -> void
    {
        if (!detail::g_attached.exchange(false, std::memory_order_acq_rel)) { return; }
        (void)::SetConsoleCtrlHandler(&detail::ctrl_handler, FALSE);
        if (const HWND window{ ::GetConsoleWindow() }; window != nullptr)
        {
            // GetSystemMenu(revert = TRUE) rebuilds the default menu, which puts
            // SC_CLOSE back.
            (void)::GetSystemMenu(window, TRUE);
            (void)::DrawMenuBar(window);
        }
        (void)::FreeConsole();
    }
}
