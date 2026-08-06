#pragma once

// chatwire.sdk — the ONLY module that knows vmhook exists.
//
// ===========================================================================
// WHY THE BOUNDARY IS HERE
// ===========================================================================
// Two reasons, one architectural and one brutally practical.
//
// ARCHITECTURAL: everything above this line — the chat feature, the protocol,
// the server — is about behaviour, not about how you reach into a JVM.  Keeping
// vmhook behind a facade means a feature is written in terms of "send this chat
// message", never in terms of klasses, oops and detours.  Adding a feature does
// not require understanding vmhook.
//
// PRACTICAL: vmhook.hpp is 24,000 lines, and putting it in the global module
// fragment of more than one module is more than today's compilers can take:
//
//   * including it in several fragments makes GCC 15 report "mismatching abi
//     tags" for its std::string-holding globals — each module gets its own view;
//   * building it as its own module and importing that makes GCC 15 segfault
//     when a TU instantiates both vmhook::register_class and vmhook::hook;
//   * Clang 19 crashes outright compiling it as a module interface unit.
//
// Every one of those is a compiler bug rather than a defect in vmhook or here,
// and all three disappear when exactly ONE translation unit includes the
// header.  That constraint pushes toward the design that was better anyway.
//
// So: this file includes vmhook.hpp.  Nothing else does.  The facade below
// exposes only plain types — std::string, bool, function pointers — so no
// vmhook type ever crosses the boundary.
#include "chatwire/common.hpp"

// THE ONE PLACE vmhook IS INCLUDED.  See the header comment above for why
// that boundary matters.
#include <vmhook/vmhook.hpp>

#include "chatwire/log.hpp"
#include "chatwire/mapping.hpp"
namespace chatwire::sdk::detail
{
    namespace map = chatwire::mapping;

    /* net.minecraft.util.IChatComponent — a rich-text chat line. */
    class chat_component : public vmhook::object<chat_component>
    {
    public:
        explicit chat_component(const vmhook::oop_t oop = nullptr) noexcept
            : vmhook::object<chat_component>{ oop }
        {
        }

        /*
            @brief Calls a no-arg String-returning method on this component.
            @details
            THE METHOD IS RESOLVED FROM THE OBJECT'S RUNTIME CLASS, not from the
            wrapper's registered one.  That distinction is the whole reason this
            function exists instead of a plain get_method() call, and getting it
            wrong CRASHED MINECRAFT.

            The wrapper is registered as `IChatComponent`, because that is what
            printChatMessage's parameter is declared as.  But IChatComponent is
            an INTERFACE: `getFormattedText` on it is an abstract declaration
            with no body, and its interpreted entry is HotSpot's
            abstract-method-error stub.  vmhook's get_method() resolves from the
            REGISTERED class, so it found that stub, and invoking it through a
            synthetic call frame took the VM down.

            The object at runtime is never an IChatComponent -- it is a
            ChatComponentText, ChatComponentTranslation, or another concrete
            subclass -- and every one of them inherits a real implementation from
            ChatComponentStyle.  Asking the oop what it actually is, then walking
            THAT class's hierarchy, finds the implementation instead of the
            declaration.

            Every failure degrades to "": the caller is a detour that must not
            throw, and an unreadable chat line is not worth a crash.
        */
        [[nodiscard]] auto text_via(const std::string& method_name) const noexcept -> std::string
        {
            try
            {
                if (method_name.empty()) { return {}; }
                vmhook::oop_t const oop{ this->get_instance() };
                if (!oop) { return {}; }

                // What is this object REALLY?  Not what the wrapper claims.
                vmhook::hotspot::klass* const concrete{ vmhook::klass_from_oop(oop) };
                if (!concrete || !vmhook::hotspot::is_valid_pointer(concrete)) { return {}; }

                vmhook::hotspot::method* const m{ find_method_in_hierarchy(
                    concrete, method_name, "()Ljava/lang/String;") };
                if (!m) { return {}; }

                const vmhook::method_proxy proxy{ oop, m, "()Ljava/lang/String;" };
                const auto value{ proxy.call() };
                if (value.threw()) { return {}; }
                return value.as_string();
            }
            catch (...) { return {}; }
        }

    private:
        /*
            @brief Finds a concrete method by name+descriptor, walking supers.
            @details
            ABSTRACT METHODS ARE SKIPPED.  A class can carry an abstract
            re-declaration of a method its own superclass implements, and picking
            that up would reintroduce exactly the crash above.  The first
            NON-abstract match wins, which is what virtual dispatch would pick.

            Every dereference is pointer-validated: this runs inside a detour on
            a live JVM, where a bad read is a crash rather than an exception.
        */
        [[nodiscard]] static auto find_method_in_hierarchy(vmhook::hotspot::klass* const start,
                                                           const std::string& name,
                                                           const std::string& descriptor) noexcept
            -> vmhook::hotspot::method*
        {
            for (vmhook::hotspot::klass* k{ start };
                 k != nullptr && vmhook::hotspot::is_valid_pointer(k);
                 k = k->get_super())
            {
                const std::int32_t count{ k->get_methods_count() };
                vmhook::hotspot::method** const methods{ k->get_methods_ptr() };
                if (!methods || count <= 0) { continue; }

                for (std::int32_t i{ 0 }; i < count; ++i)
                {
                    vmhook::hotspot::method* const m{ methods[i] };
                    if (!m || !vmhook::hotspot::is_valid_pointer(m)) { continue; }
                    // JVM_ACC_ABSTRACT (0x0400).  Read through vmhook's
                    // fault-safe probe rather than dereferencing the flags
                    // pointer: this runs inside a detour, and a Method* left
                    // cold by a deopt or a class unload would AV the JVM on a
                    // raw read.  An unreadable slot reports "not found", and we
                    // treat that as abstract -- skipping a method we cannot
                    // vouch for is the safe direction.
                    bool flags_readable{ false };
                    const bool abstract{ m->safe_access_flags_test(0x0400u, flags_readable) };
                    if (!flags_readable || abstract) { continue; }
                    if (std::string{ m->get_name() } != name) { continue; }
                    if (std::string{ m->get_signature() } != descriptor) { continue; }
                    return m;
                }
            }
            return nullptr;
        }
    };

    /* net.minecraft.client.entity.EntityPlayerSP — the local player. */
    class local_player : public vmhook::object<local_player>
    {
    public:
        explicit local_player(const vmhook::oop_t oop = nullptr) noexcept
            : vmhook::object<local_player>{ oop }
        {
        }
    };

    /* net.minecraft.client.Minecraft — the client singleton. */
    class minecraft : public vmhook::object<minecraft>
    {
    public:
        explicit minecraft(const vmhook::oop_t oop = nullptr) noexcept
            : vmhook::object<minecraft>{ oop }
        {
        }
    };

    /* net.minecraft.client.gui.GuiNewChat — the chat-box hook target. */
    class gui_new_chat : public vmhook::object<gui_new_chat>
    {
    public:
        explicit gui_new_chat(const vmhook::oop_t oop = nullptr) noexcept
            : vmhook::object<gui_new_chat>{ oop }
        {
        }
    };

    /*
        Installed hooks, kept alive here and NEVER destroyed.
        ~hook_handle removes a detour; running that during static destruction or
        DLL unload would touch a JVM that may already be tearing down.
        chatwire::sdk::remove_hooks() is the explicit, correctly-timed path.
    */
    inline auto hooks() noexcept -> std::vector<vmhook::hook_handle>&
    {
        static auto* const h{ new std::vector<vmhook::hook_handle>{} };
        return *h;
    }

    /* The callback the facade installs.  A plain function pointer: no captures
       to dangle, nothing to destroy at exit, and atomic so the detour thread
       and the installer never race. */
    inline std::atomic<void (*)(const char*, const char*)> g_chat_callback{ nullptr };

    /*
        The interceptor for chat the PLAYER sends, installed the same way and for
        the same reasons.  Returns true to swallow the message -- see
        sdk::install_command_interceptor for what that means and what it costs.
    */
    inline std::atomic<bool (*)(const char*)> g_command_callback{ nullptr };


    /*
        @brief The local player as a JNI reference, or nullptr.
        @details
        The same walk as player() below -- Minecraft.theMinecraft, then
        .thePlayer -- but done entirely through JNI, so no raw oop is ever held.
        That is what makes it safe from a thread that is not inside a hook: every
        intermediate is a reference the collector tracks and updates.

        The caller releases the result with vmhook::jni_release.
    */
    [[nodiscard]] inline auto jni_player() noexcept -> void*
    {
        try
        {
            const auto mc_class{ map::resolve(map::minecraft::clazz) };
            const auto mc_field{ map::resolve(map::minecraft::the_minecraft) };
            const auto player_field{ map::resolve(map::entity_player_sp::clazz) };
            const auto the_player{ map::resolve(map::minecraft::the_player) };
            if (mc_class.empty() || mc_field.empty() || the_player.empty()) { return nullptr; }

            vmhook::hotspot::klass* const k{ vmhook::find_class(mc_class) };
            if (!k) { return nullptr; }

            // Descriptors are built from the mapping, so this works under MCP,
            // SRG and OBF without three code paths.
            const std::string mc_descriptor{ "L" + mc_class + ";" };
            void* const mc{ vmhook::jni_static_object(k, mc_field.c_str(), mc_descriptor.c_str()) };
            if (!mc) { return nullptr; }

            const std::string player_descriptor{ "L" + player_field + ";" };
            void* const player{ vmhook::jni_object_field(mc, the_player.c_str(),
                                                         player_descriptor.c_str()) };
            vmhook::jni_release(mc);
            return player;
        }
        catch (...) { return nullptr; }
    }

    /* @brief Resolves the local player, or an empty handle. */
    [[nodiscard]] inline auto player() noexcept -> vmhook::borrowed<local_player>
    {
        try
        {
            const auto mc_field{ map::resolve(map::minecraft::the_minecraft) };
            const auto player_field{ map::resolve(map::minecraft::the_player) };
            if (mc_field.empty() || player_field.empty()) { return {}; }

            auto mc_proxy{ minecraft::static_field(mc_field.c_str()) };
            if (!mc_proxy.has_value()) { return {}; }
            const auto mc{ mc_proxy->get().to_borrowed<minecraft>() };
            if (!mc) { return {}; }

            auto player_proxy{ mc->get_field(player_field.c_str()) };
            if (!player_proxy.has_value()) { return {}; }
            return player_proxy->get().to_borrowed<local_player>();
        }
        catch (...) { return {}; }
    }
}

namespace chatwire::sdk
{
    /*
        @brief Called for each chat line: (formatted, plain), both UTF-8.
        @details
        A plain function pointer, not std::function: it is installed once and
        never changes, and a function pointer cannot throw on copy or dangle
        after a lambda's captures die.  It runs INSIDE A DETOUR, so it must be
        quick and must not throw -- usually on the game thread, which is who
        normally prints chat, but on whichever thread called printChatMessage
        when that is somebody else.
    */
    using chat_callback = void (*)(const char* formatted, const char* plain);

    /*
        @brief Makes the calling thread able to CALL Java.  Any thread, anywhere.
        @details
        Reading Java never needed this: a field get is a load from an address and
        works from any thread in the process.  Calling does — a call needs a
        JavaThread, the VM-side object holding the frame anchor a GC stack-walk
        follows and the state a safepoint reads — and a native thread the VM has
        never seen has none of that.

        vmhook asks the VM to adopt this thread as a daemon.  It is idempotent
        (a thread that is already a JavaThread, such as one inside a detour, gets
        a cheap `true`), and the VM releases the thread automatically when it
        exits, so nothing has to be arranged around it.

        chatwire calls this at the top of every entry point that reaches Java,
        rather than once somewhere central, because there is no central thread to
        do it on: a WebSocket client gets a thread of its own, and that thread is
        the one that has to be able to call.

        @return false when there is no JVM to attach to, or it refused — at which
                point no Java call from this thread can be made safely, and the
                caller must report failure rather than try anyway.
    */
    [[nodiscard]] inline auto attach_thread() noexcept -> bool
    {
        return vmhook::attach_current_thread();
    }

    /*
        @brief Releases this thread from the VM, if chatwire attached it.
        @details
        A no-op on a thread vmhook did not attach — a detour thread stays the
        VM's.  Rarely needed, because an attached thread detaches itself when it
        exits; chatwire calls it on exactly one thread, the one that unloads the
        DLL, which does NOT get to exit normally.  See chatwire::stop().
    */
    inline auto detach_thread() noexcept -> void
    {
        vmhook::detach_current_thread();
    }

    /*
        @brief Whether this JVM lets chatwire call into the game at all.
        @details
        vmhook enters Java on any thread now, but it refuses on a collector that
        relocates objects CONCURRENTLY (ZGC, Shenandoah), where "no stop-the-world
        collection is running" stops meaning "nothing is moving".  Everything a
        desktop JVM picks by default -- Serial, Parallel, G1 -- is supported.

        Worth one line at start-up: a user on an unsupported collector should be
        told at inject time, not by every command failing later.
    */
    [[nodiscard]] inline auto can_call_into_game() noexcept -> bool
    {
        return vmhook::vm_capabilities().supported;
    }


    /*
        @brief Probes the JVM and decides the mapping mode.
        @return the detected mode; mapping::mode::unknown means "not a supported
                Minecraft 1.8.9", which the caller must treat as do-not-inject.
    */
    [[nodiscard]] inline auto detect_mapping() noexcept -> chatwire::mapping::mode
    {
        namespace map = chatwire::mapping;
        map::probe_result probe{};
        try
        {
            vmhook::hotspot::klass* const mcp{ vmhook::find_class(map::minecraft::clazz.mcp) };
            probe.mcp_class_present = mcp != nullptr;
            if (mcp)
            {
                probe.mcp_field_present =
                    vmhook::find_field(mcp, map::minecraft::the_minecraft.mcp).has_value();
                probe.srg_field_present =
                    vmhook::find_field(mcp, map::minecraft::the_minecraft.srg).has_value();
            }
            else
            {
                probe.obf_class_present = vmhook::find_class(map::minecraft::clazz.obf) != nullptr;
            }
        }
        catch (...) { return map::mode::unknown; }
        return map::decide(probe);
    }

    /*
        @brief Registers every wrapper under the detected mapping's class names.
        @return false when an ESSENTIAL class is missing, at which point nothing
                chatwire does can work.  A missing optional class is logged and
                skipped, so a build without GuiNewChat still sends chat.
    */
    [[nodiscard]] inline auto register_all() noexcept -> bool
    {
        namespace map = chatwire::mapping;
        namespace d   = chatwire::sdk::detail;

        const auto reg{ []<typename wrapper_t>(const char* const what, const map::name& n,
                                               wrapper_t*) noexcept -> bool
        {
            const auto class_name{ map::resolve(n) };
            if (class_name.empty())
            {
                chatwire::log::warn("no {} class name under this mapping", what);
                return false;
            }
            if (!vmhook::register_class<wrapper_t>(class_name))
            {
                chatwire::log::warn("could not register {} ('{}')", what, class_name);
                return false;
            }
            return true;
        } };

        const bool mc{ reg("Minecraft", map::minecraft::clazz,
                           static_cast<d::minecraft*>(nullptr)) };
        const bool player{ reg("EntityPlayerSP", map::entity_player_sp::clazz,
                               static_cast<d::local_player*>(nullptr)) };
        const bool component{ reg("IChatComponent", map::i_chat_component::clazz,
                                  static_cast<d::chat_component*>(nullptr)) };
        const bool chat_gui{ reg("GuiNewChat", map::gui_new_chat::clazz,
                                 static_cast<d::gui_new_chat*>(nullptr)) };

        if (!mc || !player)
        {
            chatwire::log::error("essential Minecraft classes missing; chatwire cannot run");
            return false;
        }
        if (!component) { chatwire::log::warn("IChatComponent missing; chat text may be empty"); }
        if (!chat_gui)  { chatwire::log::warn("GuiNewChat missing; incoming chat not observed"); }
        return true;
    }

    /*
        @brief Deoptimises the methods chatwire hooks.
        @details
        vmhook's deoptimize_methods_if walks EVERY loaded class.  On a modded
        client that is tens of thousands of them, and it takes seconds, so it is
        worth doing exactly once, for every hook target at once, and only for
        what is actually hooked.  That "at once" is why both targets share one
        predicate rather than getting a call each: the walk is the cost, and it
        does not get cheaper the second time.

        The deopt is necessary because the targets are hot.  printChatMessage
        runs on every message and is JIT-compiled long before anything injects;
        sendChatMessage is compiled by the time a player has typed a few lines.
        A compiled dispatch bypasses the i2i interpreter entry a vmhook detour
        patches, so without this a hook installs successfully and simply never
        fires -- which for the command interceptor would look exactly like a
        plugin that registered a command nobody types.  vmhook holds NO_COMPILE
        on a hooked Method, so the route stays put once established.

        The pump's target, Minecraft.runTick, used to be here too.  There is no
        pump (chatwire calls Java on the calling thread now; see attach_thread),
        so what walks the class graph is only ever what a feature hooks.
    */
    inline auto deoptimize_hook_targets() noexcept -> void
    {
        namespace map = chatwire::mapping;
        try
        {
            const std::string chat_class{ map::resolve(map::gui_new_chat::clazz) };
            const std::string print{ map::resolve(map::gui_new_chat::print_chat_message) };
            const std::string player_class{ map::resolve(map::entity_player_sp::clazz) };
            const std::string send{ map::resolve(map::entity_player_sp::send_chat_message) };

            const bool want_print{ !chat_class.empty() && !print.empty() };
            const bool want_send{ !player_class.empty() && !send.empty() };
            if (!want_print && !want_send) { return; }

            (void)vmhook::deoptimize_methods_if(
                [&](const std::string& class_name, vmhook::hotspot::method* m) -> bool
                {
                    if (!m) { return false; }
                    // Fetched ONCE, not once per target.  get_name() validates
                    // the Method* and builds a std::string out of a JVM symbol,
                    // and this predicate runs for every method of every loaded
                    // class -- tens of thousands of them on a modded client.
                    const std::string method_name{ m->get_name() };
                    if (want_print && class_name == chat_class && method_name == print)
                    {
                        return true;
                    }
                    return want_send && class_name == player_class && method_name == send;
                });
        }
        catch (...) { }
    }

    /*
        @brief Hooks GuiNewChat.printChatMessage so `on_chat` sees every line.
        @details
        Every line that reaches the chat box passes through that one method —
        server messages, client messages, mod output, death messages — so one
        hook catches all of it.
    */
    [[nodiscard]] inline auto install_chat_observer(const chat_callback on_chat) noexcept -> bool
    {
        namespace map = chatwire::mapping;
        namespace d   = chatwire::sdk::detail;
        try
        {
            const auto class_name{ map::resolve(map::gui_new_chat::clazz) };
            const auto method{ map::resolve(map::gui_new_chat::print_chat_message) };
            const auto component{ map::resolve(map::i_chat_component::clazz) };
            if (class_name.empty() || method.empty() || component.empty()) { return false; }

            d::g_chat_callback.store(on_chat, std::memory_order_release);

            // Deopt already done -- see deoptimize_hook_targets().

            const std::string descriptor{ "(L" + component + ";)V" };
            auto handle{ vmhook::scoped_hook<d::gui_new_chat>(
                method, descriptor,
                [](vmhook::return_value&,
                   vmhook::borrowed<d::gui_new_chat>,
                   vmhook::borrowed<d::chat_component> line) noexcept
                {
                    // Inside a detour.  Fully guarded: this frame's caller is
                    // Minecraft's interpreter, which has no handler for a C++
                    // exception, whichever thread is running it.
                    try
                    {
                        const auto cb{ d::g_chat_callback.load(std::memory_order_acquire) };
                        if (!cb || !line) { return; }
                        namespace m = chatwire::mapping;
                        const std::string formatted{
                            line->text_via(m::resolve(m::i_chat_component::get_formatted_text)) };
                        const std::string plain{
                            line->text_via(m::resolve(m::i_chat_component::get_unformatted_text)) };
                        if (formatted.empty() && plain.empty()) { return; }
                        cb(formatted.c_str(), plain.c_str());
                    }
                    catch (...) { }
                }) };

            if (!handle.installed()) { return false; }
            d::hooks().push_back(std::move(handle));
            chatwire::log::info("chat observer installed on {}.{}{}",
                                class_name, method, descriptor);
            return true;
        }
        catch (...) { return false; }
    }

    /*
        @brief Offered every line the player sends; true SWALLOWS it.
        @details
        Runs INSIDE the sendChatMessage detour, on whichever thread is sending —
        normally the game thread, since that is who processes the chat box.  It
        must be quick and must not throw.

        The return value is the whole point: true means the message never
        reaches Minecraft's body at all, so it is not transmitted and nothing on
        the server ever hears about it.  That is what makes a plugin's `/ping` a
        command rather than a message that happens to be echoed back.
    */
    using command_callback = bool (*)(const char* message) noexcept;

    /*
        @brief Hooks EntityPlayerSP.sendChatMessage so `on_typed` can eat a line.
        @details
        This is the one hook chatwire installs that can CHANGE what the game
        does, and it is worth being plain about that: every other detour
        observes.  vmhook's return_value::cancel() suppresses the method body,
        which for a void method means the call returns having done nothing --
        the packet is never built and the server never sees the line.

        WHY THIS METHOD.  It is where the client turns "the player pressed enter
        in the chat box" into "send a packet", so it is the last place a line can
        be stopped while still being exactly what the player typed.  Hooking the
        GUI would catch keystrokes instead of messages; hooking the network layer
        would catch chat that was never typed.

        WHAT ELSE ARRIVES HERE.  Everything that reaches sendChatMessage,
        including chatwire's OWN sdk::send_chat -- a client asking to say
        "/ping" gets intercepted exactly as if the player had typed it.  That is
        the honest behaviour for a command spelled "as if typed", and it is also
        how a plugin can drive another plugin, but it does mean a client can be
        answered `{"sent":true}` for a line that was swallowed on the way out.
        A plugin that must not be intercepted should not name its output after a
        registered command.

        RE-ENTRANCY.  `on_typed` may end up back in the game (a plugin answering
        on its own socket thread will call addChatMessage), but never on THIS
        thread and never inside this frame: the callback only writes to a socket
        and returns.  Nothing here calls Java, so this detour cannot recurse
        into itself.
    */
    [[nodiscard]] inline auto install_command_interceptor(const command_callback on_typed) noexcept
        -> bool
    {
        namespace map = chatwire::mapping;
        namespace d   = chatwire::sdk::detail;
        try
        {
            const auto class_name{ map::resolve(map::entity_player_sp::clazz) };
            const auto method{ map::resolve(map::entity_player_sp::send_chat_message) };
            if (class_name.empty() || method.empty()) { return false; }

            d::g_command_callback.store(on_typed, std::memory_order_release);

            // Deopt already done -- see deoptimize_hook_targets().

            constexpr std::string_view descriptor{ "(Ljava/lang/String;)V" };
            auto handle{ vmhook::scoped_hook<d::local_player>(
                method, descriptor,
                [](vmhook::return_value& ret,
                   vmhook::borrowed<d::local_player>,
                   std::string message) noexcept
                {
                    // Inside a detour, on the game thread.  Fully guarded: the
                    // frame above is Minecraft's interpreter, which has no
                    // handler for a C++ exception.
                    try
                    {
                        const auto cb{ d::g_command_callback.load(std::memory_order_acquire) };
                        if (!cb || message.empty()) { return; }
                        // cancel() AFTER the callback has decided, and nothing
                        // between the two: a throw here would leave the message
                        // going to the server, which is the safe direction.
                        if (cb(message.c_str())) { ret.cancel(); }
                    }
                    catch (...) { }
                }) };

            if (!handle.installed()) { return false; }
            d::hooks().push_back(std::move(handle));
            chatwire::log::info("command interceptor installed on {}.{}{}",
                                class_name, method, descriptor);
            return true;
        }
        catch (...) { return false; }
    }

    /*
        @brief sendChatMessage(String) — goes to the SERVER.  ANY THREAD.
        @details
        Exactly as if the player typed it, so a leading '/' runs a command.

        Callable from any thread, and that is true of Minecraft as well as of the
        VM.  `EntityPlayerSP.sendChatMessage` reaches `NetworkManager.sendPacket`,
        which is one of the few parts of the 1.8.9 client written to be called
        from anywhere: it guards its outbound queue with a ReentrantReadWriteLock
        and, when the caller is not the channel's event loop, hands the write to
        that event loop rather than doing it inline.  The game's own network
        threads depend on that, which is why it is there.

        Re-resolves the player every call rather than caching it: the local
        player is replaced on world change and on respawn, and a cached handle
        would be a stale oop the moment either happened.

        The resolve and the call are INSIDE ONE java_thread_scope, and that is not
        tidiness — it is the correctness property.  The scope is what stops a
        collection from starting between reading the player's address and
        invoking on it.
    */
    [[nodiscard]] inline auto send_chat(const std::string& text) noexcept -> bool
    {
        namespace map = chatwire::mapping;
        try
        {
            // Outside the scope: string work and table lookups, which touch no
            // Java at all.  Everything done out here is time the VM does not
            // spend waiting for this thread.
            const auto method{ map::resolve(map::entity_player_sp::send_chat_message) };
            if (method.empty()) { return false; }

            // OFF A HOOK -> JNI.  This is the whole reason the bridge exists:
            // on a JVM that publishes no safepoint signal the transition cannot
            // be reproduced from outside, and JNI's own entry does it correctly.
            // No raw oop is held anywhere below -- every reference is one the
            // collector tracks -- so nothing can move out from under the call.
            if (!chatwire::sdk::attach_thread()) { return false; }
            if (vmhook::jni_available())
            {
                void* const player{ chatwire::sdk::detail::jni_player() };
                if (!player) { return false; }

                void* const message{ vmhook::jni_string(text) };
                if (!message) { return false; }

                const bool sent{ vmhook::jni_call_void(
                    player, method.c_str(), "(Ljava/lang/String;)V", { message }) };
                vmhook::jni_release(message);
                vmhook::jni_release(player);
                return sent;
            }

            // INSIDE A HOOK (or a JVM old enough to gate soundly): the pure
            // VMStructs path, unchanged.  One scope around resolve AND call, so
            // no collection can start between finding the player and invoking on
            // it -- the property a detour has for free.
            const vmhook::java_thread_scope java{};
            if (!java) { return false; }

            auto p{ chatwire::sdk::detail::player() };
            if (!p) { return false; }
            auto proxy{ p->get_method(method.c_str()) };
            if (!proxy.has_value()) { return false; }
            return !proxy->call(text).threw();
        }
        catch (...) { return false; }
    }

    /*
        @brief addChatMessage(IChatComponent) — CLIENT-side.  ANY THREAD.
        @details
        Never transmitted; only this player sees it.  Builds a ChatComponentText
        from `text` first — allocate, then run <init> on the raw object, which is
        what `new` compiles to in Java.  The two steps stay adjacent because the
        object is UNROOTED between them: anything that could trigger a collection
        in the gap would move it out from under the constructor call.

        WHAT THIS ONE TOUCHES, OFF ITS OWN THREAD.  Unlike send_chat, the code
        this reaches was never written to be called from anywhere:
        `EntityPlayerSP.addChatMessage` ends in `GuiNewChat.setChatLine`, which
        inserts at the front of the two ArrayLists the client thread renders
        from.  Off-thread it is a data race, so it is worth being exact about
        what the race can actually do rather than waving at it:

          * `ArrayList.add(0, e)` grows into a NEW array (fully populated before
            the field is repointed), shifts with arraycopy, stores the element,
            and only THEN increments `size`.  A reader indexing below `size`
            therefore never sees a null or an out-of-range index; the worst it
            observes is one element duplicated for the microseconds the shift is
            in flight — a chat line drawn twice for a single frame.
          * The 100-line trim removes from the TAIL.  `drawChat` reads from the
            head and stops at the visible line count, roughly twenty, so the
            reader's indices and the trim's are never near each other.
          * The line-splitting on the way in only READS the font renderer's
            width tables.

        So this races, and the failure mode is a cosmetic one-frame artefact.
        That is a MINECRAFT-level race and a separate question from the VM-level
        one the scope closes; the scope keeps the heap sound, and this
        paragraph is about the game's own unguarded lists.  At one call per
        client request a flicker is not worth a detour in the client's main loop.
        If it ever needs to be exact, the way to do it without a hook is to hand
        Minecraft an S02PacketChat and let `PacketThreadUtil` schedule it — the
        client already marshals its own inbound chat that way.

        THE ALLOCATION IS INSIDE THE SCOPE, and has to be.  A TLAB is a lockless
        bump pointer and `new_object` writes one; worse, the object is UNROOTED
        between the allocation and <init>.  A collection landing anywhere in
        allocate → construct → call would move it out from under the next step.
        Holding the gate across all three is what makes them one indivisible
        piece of Java work, exactly as they would be inside a detour.
    */
    [[nodiscard]] inline auto add_chat(const std::string& text) noexcept -> bool
    {
        namespace map = chatwire::mapping;
        namespace d   = chatwire::sdk::detail;
        try
        {
            // Everything resolvable is resolved BEFORE the gate is taken.  These
            // are metaspace and table lookups -- no oop is touched, so no
            // collection can invalidate them -- and find_class walks the whole
            // ClassLoaderDataGraph on a miss, which on a modded client is
            // seconds.  Doing that with the gate held would stop the game dead.
            // warm_up() pays the first one at start-up, where nothing waits.
            const auto class_name{ map::resolve(map::chat_component_text::clazz) };
            const auto method{ map::resolve(map::entity_player_sp::add_chat_message) };
            if (class_name.empty() || method.empty()) { return false; }
            vmhook::hotspot::klass* const k{ vmhook::find_class(class_name) };
            if (!k) { return false; }

            // <init> is spelled <init> under every mapping — the JVM reserves
            // the name, so no remapper touches it.  That is why this one lookup
            // needs no mapping entry.
            const std::string ctor_descriptor{ "(Ljava/lang/String;)V" };
            vmhook::hotspot::method** const methods{ k->get_methods_ptr() };
            const std::int32_t count{ k->get_methods_count() };
            if (!methods || count <= 0) { return false; }

            vmhook::hotspot::method* ctor_method{ nullptr };
            for (std::int32_t i{ 0 }; i < count; ++i)
            {
                vmhook::hotspot::method* const m{ methods[i] };
                if (!m || !vmhook::hotspot::is_valid_pointer(m)) { continue; }
                if (std::string{ m->get_name() } != "<init>") { continue; }
                if (std::string{ m->get_signature() } != ctor_descriptor) { continue; }
                ctor_method = m;
                break;
            }
            if (!ctor_method) { return false; }

            // OFF A HOOK -> JNI, for the reason given in send_chat.  It also
            // disposes of two hazards the pure path had here: the component is
            // allocated BY THE VM rather than by bumping someone else's TLAB,
            // and it is held in a tracked reference across <init>, so a
            // collection triggered by the constructor cannot move it out from
            // under the call that follows.
            if (!chatwire::sdk::attach_thread()) { return false; }
            if (vmhook::jni_available())
            {
                void* const message{ vmhook::jni_string(text) };
                if (!message) { return false; }

                void* const component{ vmhook::jni_new_object(
                    k, "(Ljava/lang/String;)V", { message }) };
                vmhook::jni_release(message);
                if (!component) { return false; }

                void* const player{ chatwire::sdk::detail::jni_player() };
                if (!player) { vmhook::jni_release(component); return false; }

                const std::string descriptor{ "(L" + map::resolve(map::i_chat_component::clazz)
                                              + ";)V" };
                const bool added{ vmhook::jni_call_void(
                    player, method.c_str(), descriptor.c_str(), { component }) };
                vmhook::jni_release(component);
                vmhook::jni_release(player);
                return added;
            }

            // INSIDE A HOOK: the pure VMStructs path, unchanged.
            const vmhook::java_thread_scope java{};
            if (!java) { return false; }

            auto component{ vmhook::new_object<d::chat_component>(k, k->get_instance_size()) };
            if (!component) { return false; }

            const vmhook::method_proxy ctor{ component.raw_unsafe(), ctor_method,
                                             ctor_descriptor };
            if (ctor.call(text).threw()) { return false; }

            auto p{ d::player() };
            if (!p) { return false; }
            auto proxy{ p->get_method(method.c_str()) };
            if (!proxy.has_value()) { return false; }
            return !proxy->call(component).threw();
        }
        catch (...) { return false; }
    }

    /* @brief One entry of the player list: who they are, twice. */
    struct player_identity
    {
        std::string name{};
        std::string uuid{};
    };

    /*
        @brief Everyone the client currently knows about.  ANY THREAD.
        @details
        Reads `Minecraft.theWorld.playerEntities` and, for each entry, calls
        `getName()` and `getUniqueID().toString()`.  Both come from the same
        object in the same pass, so a name and a UUID in one entry always belong
        together -- which they would not if a caller had to ask twice.

        WHY THIS IS THE LIST IT IS.  `playerEntities` is what the CLIENT has
        loaded, so it is the players near enough to be entities: it is not the
        server's full roster, and on a big server it is a small fraction of the
        tab list.  That is a property of Minecraft rather than a limitation here,
        and the honest name for the command is the field it reads.

        Entirely JNI on the off-hook path, so every intermediate is a reference
        the collector tracks -- there is no raw oop held across the many calls
        this makes, which matters more here than anywhere else in the sdk because
        the loop is long enough for a collection to be likely rather than
        possible.

        @return the players, or an empty vector when not in a world.
    */
    [[nodiscard]] inline auto players() noexcept -> std::vector<player_identity>
    {
        namespace map = chatwire::mapping;
        std::vector<player_identity> out;
        try
        {
            if (!chatwire::sdk::attach_thread() || !vmhook::jni_available()) { return out; }

            const auto mc_class{ map::resolve(map::minecraft::clazz) };
            const auto mc_field{ map::resolve(map::minecraft::the_minecraft) };
            const auto world_field{ map::resolve(map::minecraft::the_world) };
            const auto list_field{ map::resolve(map::world::player_entities) };
            const auto name_method{ map::resolve(map::entity::get_name) };
            const auto uuid_method{ map::resolve(map::entity::get_unique_id) };
            const auto world_class{ map::resolve(map::world_client::clazz) };
            if (mc_class.empty() || mc_field.empty() || world_field.empty()
                || list_field.empty() || name_method.empty() || uuid_method.empty())
            {
                return out;
            }

            vmhook::hotspot::klass* const k{ vmhook::find_class(mc_class) };
            if (!k) { return out; }

            void* const mc{ vmhook::jni_static_object(k, mc_field.c_str(),
                                                      ("L" + mc_class + ";").c_str()) };
            if (!mc) { return out; }

            void* const world{ vmhook::jni_object_field(mc, world_field.c_str(),
                                                        ("L" + world_class + ";").c_str()) };
            vmhook::jni_release(mc);
            if (!world) { return out; }          // title screen: no world, no players

            void* const list{ vmhook::jni_object_field(world, list_field.c_str(),
                                                       "Ljava/util/List;") };
            vmhook::jni_release(world);
            if (!list) { return out; }

            const std::int32_t count{ vmhook::jni_call_int(list, "size", "()I") };
            for (std::int32_t i{ 0 }; i < count && i < 1024; ++i)
            {
                // jvalue is a union whose first member is the 32-bit int, so an
                // index rides in the low half of one 8-byte slot.
                void* const index{ reinterpret_cast<void*>(static_cast<std::intptr_t>(i)) };
                void* const entry{ vmhook::jni_call_object(
                    list, "get", "(I)Ljava/lang/Object;", { index }) };
                if (!entry) { continue; }

                player_identity who{};

                if (void* const name{ vmhook::jni_call_object(
                        entry, name_method.c_str(), "()Ljava/lang/String;") })
                {
                    who.name = vmhook::jni_to_string(name);
                    vmhook::jni_release(name);
                }

                if (void* const uuid{ vmhook::jni_call_object(
                        entry, uuid_method.c_str(), "()Ljava/util/UUID;") })
                {
                    if (void* const text{ vmhook::jni_call_object(
                            uuid, "toString", "()Ljava/lang/String;") })
                    {
                        who.uuid = vmhook::jni_to_string(text);
                        vmhook::jni_release(text);
                    }
                    vmhook::jni_release(uuid);
                }

                vmhook::jni_release(entry);
                if (!who.name.empty() || !who.uuid.empty()) { out.push_back(std::move(who)); }
            }

            vmhook::jni_release(list);
        }
        catch (...) { }
        return out;
    }

    /*
        @brief True when the local player exists, i.e. we are in a world.
        @details
        ANY THREAD, and with no gate.  Resolving the player is a static-field
        read followed by an instance-field read, and reading Java has never
        needed the VM's permission — only calling does.  The answer is a
        null test, and a collection moving the player changes its address but
        never makes it null, so the result cannot be wrong in a way that
        matters.  Deliberately NOT taking the gate: this is polled, and the gate
        is a thing the whole VM waits on.
    */
    [[nodiscard]] inline auto in_world() noexcept -> bool
    {
        return static_cast<bool>(chatwire::sdk::detail::player());
    }

    /*
        @brief Pays every first-time resolution cost up front.
        @details
        Call once at start-up, on a thread with nothing waiting on it.

        This exists because of what a java_thread_scope costs while it is open: the VM
        cannot reach a safepoint, so every microsecond inside one is a
        microsecond the game is not running.  vmhook's class, field and method
        lookups are cached, but the FIRST of each walks the ClassLoaderDataGraph
        — every loaded class, tens of thousands of them on a modded client,
        seconds of work.  That must never happen inside a scope, and the way to
        guarantee it never does is to have already done it out here.

        Every lookup below is a read; nothing is called, so no gate is needed and
        the game is not disturbed.  Failures are not reported: a name that will
        not resolve now will fail the same way at call time, where there is a
        client to tell about it.
    */
    inline auto warm_up() noexcept -> void
    {
        namespace map = chatwire::mapping;
        try
        {
            // vmhook's capability probe walks the JVM flag table once (~1 ms);
            // paying it here keeps it out of the first java_thread_scope, where
            // the whole VM would be waiting for it.
            (void)vmhook::vm_capabilities();

            // The static + instance field chain behind player(), and the klass
            // and <init> behind add_chat.
            (void)chatwire::sdk::in_world();
            const auto component{ map::resolve(map::chat_component_text::clazz) };
            if (!component.empty()) { (void)vmhook::find_class(component); }
        }
        catch (...) { }
    }

    /*
        @brief Removes every installed hook.
        @details
        Explicit rather than destructor-driven: unhooking touches the JVM, and
        doing that from static destruction or DLL unload would reach a VM that
        may already be tearing down.
    */
    inline auto remove_hooks() noexcept -> void
    {
        namespace d = chatwire::sdk::detail;
        d::g_chat_callback.store(nullptr, std::memory_order_release);
        // Cleared BEFORE the hooks come down, so a detour that is already
        // running finds no callback and lets the message through rather than
        // swallowing it on the way out.  A half-detached chatwire that eats the
        // player's chat would be the worst possible parting gift.
        d::g_command_callback.store(nullptr, std::memory_order_release);
        try { d::hooks().clear(); } catch (...) { }
    }
}
