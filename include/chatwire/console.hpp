// chatwire/console.hpp — the console attached to the injected game.
//
// ===========================================================================
// TWO PLATFORMS, TWO VERY DIFFERENT PROBLEMS
// ===========================================================================
// On Windows a game launched through javaw.exe has NO console at all, so one
// has to be created before anything can be printed -- and a created console
// brings a close button that kills the game, which is the whole subject of the
// next section.
//
// On Linux and macOS none of that applies, and the difference is not a gap in
// the port.  A process cannot conjure a terminal for itself: it inherits the
// one it was launched from or it has none, and a library injected into someone
// else's process has no business opening a window.  So on POSIX chatwire is
// ALWAYS a guest on the game's own stdout -- which is precisely the "borrowed
// console" case this file already had to handle on Windows, for a game started
// from java.exe rather than javaw.exe.  It prints, and that is all: it never
// reads stdin (that belongs to whatever launched the game) and it installs no
// signal handler (process-wide signal disposition belongs to the host program,
// for the same reason chatwire suppresses SIGPIPE per-socket rather than
// per-process -- see net.hpp).  `detach` there lives on the websocket.
//
// ===========================================================================
// WHY THE X IS DISABLED, AND WHAT TO DO INSTEAD  (Windows)
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

#if defined(_WIN32)
    #include <windows.h>
#else
    #include <unistd.h>
#endif

namespace chatwire::console
{
    namespace detail
    {
        /* Whether the console host accepted ENABLE_VIRTUAL_TERMINAL_PROCESSING.
           When it did not, ANSI escapes must be STRIPPED rather than emitted --
           printing them raw turns every coloured line into "[92mtext[0m". */
        inline std::atomic<bool> g_colour{ false };

        /*
            @brief Removes ANSI escapes, for a console host that cannot render them.
            @details
            Only CSI sequences (ESC [ ... final byte) appear in chatwire's output, so
            that is all this understands; anything else is left alone rather than
            guessed at.
        */
        [[nodiscard]] inline auto strip_ansi(const std::string_view text) -> std::string
        {
            std::string out;
            out.reserve(text.size());
            for (std::size_t i{ 0 }; i < text.size(); ++i)
            {
                if (text[i] != '\x1b' || i + 1 >= text.size() || text[i + 1] != '[')
                {
                    out += text[i];
                    continue;
                }
                i += 2;
                while (i < text.size() && !(text[i] >= '@' && text[i] <= '~')) { ++i; }
            }
            return out;
        }

        /*
            @brief Writes one line to the console, correctly, whatever its code page.
            @details
            Via WriteConsoleW, deliberately.  chatwire may be SHARING the game's
            console -- Lunar, and any java.exe launch, already has one -- and the
            obvious fix for mojibake, SetConsoleOutputCP(CP_UTF8), changes the code
            page for the WHOLE console, the game's own logger included.  Fixing our
            output by corrupting theirs is not a fix.

            WriteConsoleW takes UTF-16 and consults no code page at all, so the
            section sign and any non-ASCII player name arrive intact while nothing
            else is disturbed.

            Falls back to stdout when there is no console handle, which is the case
            for the tools that share this header but run in an ordinary terminal.

            None of this exists off Windows: a POSIX terminal has been UTF-8 for
            decades and takes bytes, so the fallback path IS the implementation
            there.  The only decision left is whether to keep the escapes, and
            that is answered by asking whether stdout is a terminal at all --
            chatwire's output is often being redirected into the launcher's log.
        */
        inline auto write_line(const std::string_view utf8) noexcept -> void
        {
            try
            {
                const std::string text{ detail::g_colour.load(std::memory_order_acquire)
                                            ? std::string{ utf8 }
                                            : strip_ansi(utf8) };

#if !defined(_WIN32)
                std::fprintf(stdout, "%.*s\n", static_cast<int>(text.size()), text.data());
                std::fflush(stdout);
#else
                const HANDLE out{ ::GetStdHandle(STD_OUTPUT_HANDLE) };
                DWORD        mode{ 0 };
                if (out == INVALID_HANDLE_VALUE || out == nullptr || !::GetConsoleMode(out, &mode))
                {
                    std::fprintf(stdout, "%.*s\n", static_cast<int>(text.size()), text.data());
                    std::fflush(stdout);
                    return;
                }

                const int wide_len{ ::MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                                          static_cast<int>(text.size()),
                                                          nullptr, 0) };
                if (wide_len < 0) { return; }

                std::wstring wide(static_cast<std::size_t>(wide_len), L'\0');
                if (wide_len > 0)
                {
                    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                          wide.data(), wide_len);
                }
                wide += L'\n';

                DWORD written{ 0 };
                (void)::WriteConsoleW(out, wide.data(), static_cast<DWORD>(wide.size()),
                                      &written, nullptr);
#endif
            }
            catch (...) { }
        }


        /* What `detach` should call.  Set by attach(); a plain function pointer
           so there is nothing to destroy at exit. */
        inline std::atomic<void (*)()> g_on_detach{ nullptr };
        inline std::atomic<bool>       g_attached{ false };
        inline std::atomic<bool>       g_stopping{ false };

        /*
            @brief Whether AllocConsole gave us this console, or it was the
                   game's already.
            @details
            The difference decides what release() is allowed to do.  A console we
            allocated is ours to take apart; one the game was launched with --
            Lunar has one, as does any java.exe start -- is not.  Calling
            FreeConsole on a borrowed console detaches the WHOLE PROCESS from it,
            the game's own stdout and stderr included, which is a fine way to
            break or kill a game that was doing nothing wrong.
        */
        inline std::atomic<bool> g_owned{ false };

        /*
            @brief The console command reader, kept JOINABLE on purpose.
            @details
            It used to be detached, and that was a crash.  The unload path ends in
            FreeLibraryAndExitThread, which frees the module out from under every
            thread except the one that calls it -- and this one spends its life
            parked in fgets(), inside that module.  It survived the unload asleep
            and died the moment anything closed stdin and woke it into pages that
            no longer held any code.
        */
        inline std::thread     g_commands{};
        inline std::thread::id g_commands_id{};

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

#if defined(_WIN32)
        /*
            @brief The safety net for closes chatwire cannot refuse.
            @details
            Windows only, and deliberately not mirrored with a SIGINT handler on
            POSIX.  This one is installed on a console chatwire CREATED and owns;
            a POSIX signal disposition is process-wide and belongs to the game,
            and an injected library that quietly took over SIGINT would break
            Ctrl+C for whoever launched it.
        */
        inline auto WINAPI ctrl_handler(const DWORD event) -> BOOL
        {
            switch (event)
            {
            case CTRL_C_EVENT:
            case CTRL_BREAK_EVENT:
            case CTRL_CLOSE_EVENT:
            case CTRL_LOGOFF_EVENT:
            case CTRL_SHUTDOWN_EVENT:
                write_line("\x1b[93m  console closing - detaching chatwire...\x1b[0m");
                do_detach();
                return TRUE;
            default:
                return FALSE;
            }
        }
#endif

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
                    write_line(
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
                    write_line("  \x1b[90mverbose logging on\x1b[0m");
                    continue;
                }
                if (command == "quiet")
                {
                    chatwire::log::set_level(chatwire::log::level::warning);
                    write_line("  \x1b[90mverbose logging off\x1b[0m");
                    continue;
                }
                write_line("  \x1b[90munknown command; try 'help'\x1b[0m");
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

#if !defined(_WIN32)
        // POSIX: there is nothing to allocate and nothing to own.  chatwire
        // writes to whatever stdout the game was given -- see the file header --
        // so the only question is whether escapes will be rendered or read as
        // literal text by whatever is capturing that stream.
        (void)on_detach;
        detail::g_owned.store(false, std::memory_order_release);
        detail::g_colour.store(::isatty(STDOUT_FILENO) == 1, std::memory_order_release);
        detail::g_attached.store(true, std::memory_order_release);
        chatwire::log::warn("console: sharing the game's output - chat will appear "
                            "here, but send system.detach over the websocket to "
                            "unload");
        return true;
#else
        const bool had_console{ ::GetConsoleWindow() != nullptr };
        if (!had_console && !::AllocConsole()) { return false; }
        const bool owned{ !had_console };
        detail::g_owned.store(owned, std::memory_order_release);

        FILE* stream{ nullptr };
        (void)::freopen_s(&stream, "CONOUT$", "w", stdout);
        (void)::freopen_s(&stream, "CONOUT$", "w", stderr);
        (void)::freopen_s(&stream, "CONIN$",  "r", stdin);

        // Try for colour, and REMEMBER whether we got it.  An old console host
        // rejects the flag, and emitting escapes it cannot render is worse than
        // plain text: every line becomes "[92mtext[0m".
        if (const HANDLE out{ ::GetStdHandle(STD_OUTPUT_HANDLE) };
            out != INVALID_HANDLE_VALUE && out != nullptr)
        {
            DWORD mode{ 0 };
            if (::GetConsoleMode(out, &mode))
            {
                const bool ok{ ::SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)
                               != 0 };
                detail::g_colour.store(ok, std::memory_order_release);
            }
        }
        // Deliberately NOT SetConsoleOutputCP -- see write_line.  The code page
        // belongs to the whole console, which we may be sharing with the game's
        // own logger, and WriteConsoleW needs no code page at all.

        detail::g_attached.store(true, std::memory_order_release);

        // Everything past this point ALTERS the console rather than writing to
        // it, and none of it is ours to do to a window the game was launched
        // with.  On a borrowed console chatwire is a guest: it prints, and that
        // is all.  `detach` there lives on the websocket instead.
        if (!owned)
        {
            chatwire::log::warn("console: sharing the game's window - chat will "
                                "appear here, but type 'detach' into the client, "
                                "not here");
            return true;
        }

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

        try
        {
            // Joinable, and joined by release().  See g_commands for what a
            // detached reader costs.
            detail::g_commands    = std::thread{ &detail::command_loop };
            detail::g_commands_id = detail::g_commands.get_id();
        }
        catch (...)
        {
            // No command thread: the console still shows chat, and `detach` is
            // simply unavailable.  Not worth failing start-up over.
            chatwire::log::warn("console: could not start the command reader; "
                                "'detach' will not work");
        }
        return true;
#endif
    }

    /* @brief Writes one line to the console.  See detail::write_line. */
    inline auto write_line(const std::string_view utf8) noexcept -> void
    {
        detail::write_line(utf8);
    }

    /* @brief The banner.  Not a diagnostic, so it bypasses the level filter. */
    inline auto banner(const std::string_view version, const std::uint16_t port,
                       const std::string_view mapping) noexcept -> void
    {
        try
        {
            write_line("");
            write_line(std::format(
                "  \x1b[92mchatwire {}\x1b[0m  \x1b[90m|\x1b[0m  ws://127.0.0.1:{}"
                "  \x1b[90m|\x1b[0m  {}", version, port, mapping));
            if (detail::g_owned.load(std::memory_order_acquire))
            {
                write_line(
                    "  \x1b[90mchat appears below.  type 'help' for commands, "
                    "'detach' to unload.\x1b[0m");
                write_line(
                    "  \x1b[90m(the close button is disabled on purpose - closing a "
                    "console kills the game)\x1b[0m");
            }
            else
            {
                write_line(
                    "  \x1b[90mchat appears below.  this is the game's own window, "
                    "so chatwire only prints here -\x1b[0m");
                write_line(
                    "  \x1b[90msend system.detach over the websocket to "
                    "unload.\x1b[0m");
            }
            write_line("");
        }
        catch (...) { }
    }

    /* @brief Prints one chat line, colours and all. */
    inline auto chat_line(const std::string_view formatted) noexcept -> void
    {
        try { write_line(chatwire::ansi::render(formatted)); }
        catch (...) { }
    }

    /* @brief A chatwire event, visually distinct from game chat. */
    inline auto event(const std::string_view text) noexcept -> void
    {
        try { write_line(std::format("  \x1b[90m* {}\x1b[0m", text)); }
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

#if !defined(_WIN32)
        // Nothing was taken, so there is nothing to give back: no console was
        // allocated, no handler installed, and no reader started on a stdin that
        // belongs to whoever launched the game.
        return;
#else
        // Unhook the CTRL handler before anything else: it is a function in this
        // module, and the module is about to stop existing.
        (void)::SetConsoleCtrlHandler(&detail::ctrl_handler, FALSE);

        // Retire the command reader, and WAIT for it.  The flag above is enough
        // to make it want to stop but not enough to make it stop: it is parked in
        // fgets() and will not look at the flag until a line arrives.  So a line
        // is delivered -- a synthetic Return, straight into the console's input
        // buffer -- and then it is joined.
        //
        // The join is the point.  It is what turns "the reader will exit soon"
        // into "the reader has exited", and only the second is safe to unload a
        // module on.  See g_commands.
        if (detail::g_commands.joinable())
        {
            if (const HANDLE in{ ::GetStdHandle(STD_INPUT_HANDLE) };
                in != INVALID_HANDLE_VALUE && in != nullptr)
            {
                INPUT_RECORD record{};
                record.EventType                        = KEY_EVENT;
                record.Event.KeyEvent.bKeyDown          = TRUE;
                record.Event.KeyEvent.wRepeatCount      = 1;
                record.Event.KeyEvent.wVirtualKeyCode   = VK_RETURN;
                record.Event.KeyEvent.uChar.UnicodeChar = L'\r';

                DWORD written{ 0 };
                (void)::WriteConsoleInputW(in, &record, 1u, &written);
            }

            // Joining from the reader itself would deadlock.  Nothing does that
            // today -- every detach path runs on a thread of its own -- but the
            // failure mode is a hung game, so it is checked rather than assumed.
            if (detail::g_commands_id == std::this_thread::get_id())
            {
                detail::g_commands.detach();
            }
            else
            {
                try { detail::g_commands.join(); } catch (...) { }
            }
        }

        if (!detail::g_owned.load(std::memory_order_acquire))
        {
            // A borrowed console: leave it exactly as found.  In particular do
            // NOT call FreeConsole, which would detach the GAME from its own
            // console along with us.
            return;
        }

        if (const HWND window{ ::GetConsoleWindow() }; window != nullptr)
        {
            // GetSystemMenu(revert = TRUE) rebuilds the default menu, which puts
            // SC_CLOSE back.
            (void)::GetSystemMenu(window, TRUE);
            (void)::DrawMenuBar(window);
        }
        (void)::FreeConsole();
#endif
    }
}
