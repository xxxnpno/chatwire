#pragma once

// chatwire.core.pump — the ONE place this process is allowed to enter Java.
//
// ===========================================================================
// THE RULE THIS EXISTS TO ENFORCE
// ===========================================================================
// vmhook's contract: method_proxy::call() and make_java_string() are only valid
// on a real JavaThread that is *inside an interpreter detour* — where
// current_java_thread is set, a last_Java_frame anchor exists, and the thread
// state is safepoint-consistent.  Calling the raw call stub from a plain native
// thread crashes the VM, because a GC stack-walk faults on the missing anchor.
//
// chatwire has a WebSocket thread that receives "send this chat message" from a
// client.  That thread must never touch Java.  Instead it hands a task to the
// pump, and the pump runs it from inside a detour on Minecraft's own client
// thread.
//
//     websocket thread                 minecraft client thread
//     ----------------                 -----------------------
//     submit(task)      ------->       Minecraft.runTick() fires
//     (returns immediately)            detour drains the queue and RUNS the task
//
// Minecraft.runTick is the natural pump: it is called every client tick (~20/s
// in game, faster when the frame rate allows), on the thread that owns the
// world, so the latency is a fraction of a tick and nothing else has to be
// arranged.
//
// ===========================================================================
// STABILITY PROPERTIES, AND WHY EACH ONE IS HERE
// ===========================================================================
//   * BOUNDED QUEUE.  A client that spams faster than the game ticks must not
//     grow this without limit.  Over the cap, the OLDEST task is dropped and
//     counted — dropping is a bounded, visible failure; growing is an
//     out-of-memory in someone's game.
//   * NO EXCEPTION ESCAPES.  drain() is noexcept and wraps every task in a
//     catch-all.  An exception unwinding out of a detour reaches the JVM's
//     interpreter frame, which has no handler for it.
//   * FIRE-AND-FORGET.  submit() copies what the task needs and returns; it
//     never blocks the socket thread on the game thread.  A task that captured
//     a reference to a socket-thread local would dangle, so tasks own their
//     data by value.
//   * SURVIVES A DEAD GAME.  If the pump detour never fires (menu screen, game
//     hung, hook failed), tasks accumulate to the cap and are dropped.  Nothing
//     blocks and nothing waits forever.
#include "chatwire/common.hpp"

#include "chatwire/log.hpp"
namespace chatwire::pump
{
    /*
        @brief A unit of work to run on the Minecraft client thread.
        @details
        Must own everything it touches.  It runs later, on another thread, after
        the submitting frame is long gone.
    */
    using task = std::function<void()>;

    /* @brief How many tasks may wait before the oldest are dropped. */
    inline constexpr std::size_t max_pending{ 256u };

    namespace detail
    {
        inline auto queue_mutex() noexcept -> std::mutex&
        {
            static std::mutex* const m{ new std::mutex{} };
            return *m;
        }

        inline auto queue() noexcept -> std::deque<task>&
        {
            static auto* const q{ new std::deque<task>{} };
            return *q;
        }

        inline std::atomic<std::uint64_t> g_submitted{ 0 };
        inline std::atomic<std::uint64_t> g_executed{ 0 };
        inline std::atomic<std::uint64_t> g_dropped{ 0 };
        inline std::atomic<std::uint64_t> g_failed{ 0 };
        inline std::atomic<bool>          g_accepting{ true };
    }

    /*
        @brief Queues `work` to run on the Minecraft client thread.
        @details
        Returns immediately; never blocks and never runs `work` inline.  Safe
        from any thread.

        @return false when the task was dropped — either because the queue was
                full or because the pump is shutting down.  Callers should
                report that rather than assume delivery.
    */
    inline auto submit(task work) noexcept
        -> bool
    {
        if (!work) { return false; }
        if (!detail::g_accepting.load(std::memory_order_acquire)) { return false; }

        try
        {
            const std::lock_guard<std::mutex> guard{ detail::queue_mutex() };
            auto& q{ detail::queue() };

            // Drop the OLDEST, not the newest: for a chat bridge the newest
            // message is the one the user just asked for, and a stale backlog
            // is what should give way.
            while (q.size() >= max_pending)
            {
                q.pop_front();
                detail::g_dropped.fetch_add(1, std::memory_order_relaxed);
            }
            q.push_back(std::move(work));
            detail::g_submitted.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        catch (...)
        {
            // Allocation failure enqueuing.  Losing the task is survivable;
            // propagating out of a socket handler is not.
            detail::g_dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }

    /*
        @brief Runs every queued task.  CALL ONLY FROM INSIDE A DETOUR.
        @details
        Drains under a snapshot rather than holding the lock across execution: a
        task that submits another task would otherwise deadlock on a
        non-recursive mutex, and holding the queue lock while calling into Java
        would block the socket thread for the length of a JVM call.

        Every task is individually guarded.  One task throwing must not skip the
        rest and must not reach the interpreter frame above us.

        Complexity: O(queued).  Exception safety: noexcept, absolutely.
    */
    inline auto drain() noexcept
        -> void
    {
        std::deque<task> batch;
        try
        {
            const std::lock_guard<std::mutex> guard{ detail::queue_mutex() };
            auto& q{ detail::queue() };
            if (q.empty()) { return; }        // the overwhelmingly common tick
            batch.swap(q);
        }
        catch (...)
        {
            return;
        }

        for (auto& work : batch)
        {
            try
            {
                work();
                detail::g_executed.fetch_add(1, std::memory_order_relaxed);
            }
            catch (const std::exception& ex)
            {
                detail::g_failed.fetch_add(1, std::memory_order_relaxed);
                chatwire::log::error("pump task threw: {}", ex.what());
            }
            catch (...)
            {
                detail::g_failed.fetch_add(1, std::memory_order_relaxed);
                chatwire::log::error("pump task threw a non-std exception");
            }
        }
    }

    /*
        @brief Stops accepting work and discards what is queued.
        @details
        Called during shutdown, BEFORE the hooks come down.  A task still in the
        queue when its detour is unhooked would never run; worse, one that runs
        while the DLL is unloading executes freed code.  Refusing new work first
        and clearing second closes both.
    */
    inline auto shutdown() noexcept
        -> void
    {
        detail::g_accepting.store(false, std::memory_order_release);
        try
        {
            const std::lock_guard<std::mutex> guard{ detail::queue_mutex() };
            detail::queue().clear();
        }
        catch (...)
        {
        }
    }

    /* @brief Re-opens the pump after shutdown().  For tests and re-injection. */
    inline auto reopen() noexcept -> void
    {
        detail::g_accepting.store(true, std::memory_order_release);
    }

    /* @brief Counters, for the `stats` protocol command and for diagnosing a
       pump whose detour never fires (submitted high, executed zero). */
    struct stats
    {
        std::uint64_t submitted{ 0 };
        std::uint64_t executed{ 0 };
        std::uint64_t dropped{ 0 };
        std::uint64_t failed{ 0 };
        std::size_t   pending{ 0 };
    };

    [[nodiscard]] inline auto snapshot() noexcept
        -> stats
    {
        stats out{};
        out.submitted = detail::g_submitted.load(std::memory_order_relaxed);
        out.executed  = detail::g_executed.load(std::memory_order_relaxed);
        out.dropped   = detail::g_dropped.load(std::memory_order_relaxed);
        out.failed    = detail::g_failed.load(std::memory_order_relaxed);
        try
        {
            const std::lock_guard<std::mutex> guard{ detail::queue_mutex() };
            out.pending = detail::queue().size();
        }
        catch (...)
        {
        }
        return out;
    }
}
