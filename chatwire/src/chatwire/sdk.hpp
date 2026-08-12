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
// PRACTICAL: vmhook.hpp is 27,000 lines, and putting it in the global module
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
    };

    /*
        @brief A no-arg String-returning method on a chat line, or "".
        @details
        This used to be sixty lines that asked the oop what class it really was
        and walked THAT hierarchy skipping abstract methods, because the wrapper
        is registered as IChatComponent -- an INTERFACE -- and resolving
        getFormattedText from the registered class found the interface's abstract
        declaration, whose entry is HotSpot's AbstractMethodError stub.  Calling
        it took Minecraft down every time.

        vmhook does that itself now (>= 6.0.0): get_method resolves from the
        object's runtime class and never returns an abstract method.  What is
        left here is the part that was always this file's business -- a missing
        mapping name, and a detour that must not throw.

        Every failure degrades to "": an unreadable chat line is not worth a
        crash in a frame whose caller is Minecraft's interpreter.
    */
    [[nodiscard]] inline auto text_of(const std::unique_ptr<chat_component>& line,
                                      const std::string& method_name) noexcept
        -> std::string
    {
        try
        {
            if (method_name.empty() || !line->get_instance()) { return {}; }
            const auto method{ line->get_method(method_name) };
            if (!method) { return {}; }
            return method->call();
        }
        catch (...) { return {}; }
    }

    /*
        net.minecraft.util.ChatComponentText — the CONCRETE chat line chatwire
        builds.  Separate from chat_component, which is registered as the
        INTERFACE the game's own methods are declared in terms of: an interface
        cannot be instantiated, and vmhook::make_unique refuses it by construction.
    */
    class chat_component_text : public vmhook::object<chat_component_text>
    {
    public:
        explicit chat_component_text(const vmhook::oop_t oop = nullptr) noexcept
            : vmhook::object<chat_component_text>{ oop }
        {
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
        net.minecraft.client.multiplayer.WorldClient — the argument of
        loadWorld, and the ONE wrapper here that is never register_class'd.
        It does not need to be: a detour argument declared as
        std::unique_ptr<W> is wrapped directly around the incoming oop, because
        W is known statically at the hook site.  Registration only buys field
        and method lookups, and nothing here does either -- the world is read
        for one thing, whether it is null.
    */
    class world_client : public vmhook::object<world_client>
    {
    public:
        explicit world_client(const vmhook::oop_t oop = nullptr) noexcept
            : vmhook::object<world_client>{ oop }
        {
        }
    };

    /*
        net.minecraft.util.FoodStats — hunger, reached through EntityPlayer.
        Registered like the rest because its two numbers are read by name off
        the object the player's `foodStats` field hands back.
    */
    class food_stats : public vmhook::object<food_stats>
    {
    public:
        explicit food_stats(const vmhook::oop_t oop = nullptr) noexcept
            : vmhook::object<food_stats>{ oop }
        {
        }
    };

    /*
        net.minecraft.client.gui.GuiScreen — the DECLARED type of
        Minecraft.currentScreen, and never registered: nothing is looked up on
        it.  What a caller wants is which screen it really is, and that comes
        from the object's own klass rather than from this name.
    */
    class gui_screen : public vmhook::object<gui_screen>
    {
    public:
        explicit gui_screen(const vmhook::oop_t oop = nullptr) noexcept
            : vmhook::object<gui_screen>{ oop }
        {
        }
    };

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
        The world-change observer.  `loaded` is false when the world argument was
        null, which is Minecraft's way of saying the client is LEAVING one -- a
        disconnect, or a return to the title screen.
    */
    inline std::atomic<void (*)(bool)> g_world_callback{ nullptr };


    /*
        @brief The GuiNewChat.printChatMessage detour.
        @details
        A plain function, which is what vmhook::hook wants and what this needs to
        be: it outlives install_chat_observer's frame, and a lambda with captures
        could not.  The Java arguments are reproduced as C++ ones -- the receiver
        first, then the line -- and vmhook hands each over as a unique_ptr that is
        never null, though the object it wraps may be.

        Runs INSIDE the detour, so the frame above is Minecraft's interpreter,
        which has no handler for a C++ exception whichever thread is running it.
        Hence the blanket catch: every failure here is a chat line not reported,
        not a dead game.
    */
    inline auto on_print_chat_message(vmhook::return_value&,
                                      const std::unique_ptr<gui_new_chat>&,
                                      const std::unique_ptr<chat_component>& line) noexcept
        -> void
    {
        try
        {
            const auto cb{ g_chat_callback.load(std::memory_order_acquire) };
            if (!cb || !line->get_instance()) { return; }
            const std::string formatted{
                text_of(line, map::resolve(map::i_chat_component.get_formatted_text)) };
            const std::string plain{
                text_of(line, map::resolve(map::i_chat_component.get_unformatted_text)) };
            if (formatted.empty() && plain.empty()) { return; }
            cb(formatted.c_str(), plain.c_str());
        }
        catch (...) { }
    }

    /*
        @brief The EntityPlayerSP.sendChatMessage detour.
        @details
        The one detour chatwire installs that can CHANGE what the game does:
        cancel() suppresses the method body, so the packet is never built and the
        server never hears the line.  A java.lang.String argument arrives as a
        std::string -- no decoding at the call site.

        cancel() happens AFTER the callback has decided and with nothing between
        the two: a throw in that gap would leave the message going to the server,
        which is the safe direction.
    */
    inline auto on_send_chat_message(vmhook::return_value& ret,
                                     const std::unique_ptr<local_player>&,
                                     const std::string& message) noexcept
        -> void
    {
        try
        {
            const auto cb{ g_command_callback.load(std::memory_order_acquire) };
            if (!cb || message.empty()) { return; }
            if (cb(message.c_str())) { ret.cancel(); }
        }
        catch (...) { }
    }

    /*
        @brief The Minecraft.loadWorld detour.
        @details
        The receiver and ONE argument are declared; the trailing String is not,
        and leaving it out is what lets a single detour serve both overloads.
        `loadWorld(WorldClient)` and `loadWorld(WorldClient, String)` agree on
        every slot up to the world, and vmhook reads only as many slots as the
        detour asks for -- so declaring the String would make the shorter
        overload read one that is not there.

        The world may be NULL, and that case is the interesting one: it is the
        client leaving a world rather than entering one.  A wrapper argument is
        never a null pointer -- it is a wrapper whose get_instance() is null --
        so the test is on the instance.

        Runs on whichever thread is changing world, which for a join is the
        client thread (the packet handler marshals itself onto it) and for a
        disconnect can be a netty thread.  Nothing here cares: the callback is
        loaded atomically, and the frame above is Minecraft's interpreter, which
        has no handler for a C++ exception whoever is running it.
    */
    inline auto on_load_world(vmhook::return_value&,
                              const std::unique_ptr<minecraft>&,
                              const std::unique_ptr<world_client>& world) noexcept
        -> void
    {
        try
        {
            const auto cb{ g_world_callback.load(std::memory_order_acquire) };
            if (!cb) { return; }
            cb(world->get_instance() != nullptr);
        }
        catch (...) { }
    }

    /*
        @brief WorldClient's JVM descriptor -- "Lbdb;" on a vanilla 1.8.9.
        @details
        ASKED OF THE JVM first, and taken from the mapping table only if that
        fails.  Both answers are now correct for a stock client -- the table
        carries the real OBF name -- so this is not standing in for a gap in it;
        it is the answer for a client that is NOT stock.  Lunar, Badlion and
        friends ship repackaged classes, and the declared type of
        `Minecraft.theWorld` is that jar's own spelling of WorldClient by
        construction, whatever anyone's table says.

        Metaspace reads only -- no oop is touched and nothing is called -- so
        this needs no thread state and no gate.  The result is cached by vmhook's
        field cache, so the walk happens once.

        @return the descriptor with its L and ;, or "" when neither route has an
                answer, at which point no descriptor built from it would be right
                either.
    */
    [[nodiscard]] inline auto world_client_descriptor() noexcept -> std::string
    {
        try
        {
            const auto mc_class{ map::resolve(map::minecraft.clazz) };
            const auto world_field{ map::resolve(map::minecraft.the_world) };
            if (!mc_class.empty() && !world_field.empty())
            {
                if (vmhook::hotspot::klass* const k{ vmhook::find_class(mc_class) })
                {
                    const auto field{ vmhook::find_field(k, world_field) };
                    // Anything but an object type means the mapping found a
                    // DIFFERENT field of that name, and a descriptor built from
                    // an int would hook nothing.  Fall through to the table.
                    if (field && !field->signature.empty() && field->signature.front() == 'L')
                    {
                        return field->signature;
                    }
                }
            }
        }
        catch (...) { }

        // The table's answer, verified against the shipped jar for all three
        // mappings, for the case where the field lookup itself failed.
        const auto fallback{ map::resolve(map::world_client.clazz) };
        return fallback.empty() ? std::string{} : std::format("L{};", fallback);
    }

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
            const auto mc_class{ map::resolve(map::minecraft.clazz) };
            const auto mc_field{ map::resolve(map::minecraft.the_minecraft) };
            const auto player_field{ map::resolve(map::entity_player_sp.clazz) };
            const auto the_player{ map::resolve(map::minecraft.the_player) };
            if (mc_class.empty() || mc_field.empty() || the_player.empty()) { return nullptr; }

            vmhook::hotspot::klass* const k{ vmhook::find_class(mc_class) };
            if (!k) { return nullptr; }

            // Descriptors are built from the mapping, so this works under MCP,
            // SRG and OBF without three code paths.
            const std::string mc_descriptor{ std::format("L{};", mc_class) };
            void* const mc{ vmhook::jni_static_object(k, mc_field.c_str(), mc_descriptor.c_str()) };
            if (!mc) { return nullptr; }

            const std::string player_descriptor{ std::format("L{};", player_field) };
            void* const player{ vmhook::jni_object_field(mc, the_player.c_str(),
                                                         player_descriptor.c_str()) };
            vmhook::jni_release(mc);
            return player;
        }
        catch (...) { return nullptr; }
    }

    /*
        @brief Resolves the local player.  NEVER null; ->get_instance() may be.
        @details
        Minecraft.theMinecraft (a STATIC field, read through an instance-less
        wrapper exactly as an instance field would be) then .thePlayer.  Both
        reads, so no thread state is needed -- reading Java is a load from an
        address, and only calling needs the VM's permission.

        Not in a world means thePlayer is null, which is a wrapper whose
        get_instance() is null rather than an absent wrapper.
    */
    [[nodiscard]] inline auto player() noexcept -> std::unique_ptr<local_player>
    {
        try
        {
            const auto mc_field{ map::resolve(map::minecraft.the_minecraft) };
            const auto player_field{ map::resolve(map::minecraft.the_player) };
            if (mc_field.empty() || player_field.empty()) { return std::make_unique<local_player>(); }

            const auto mc_proxy{ minecraft{}.get_field(mc_field) };
            if (!mc_proxy) { return std::make_unique<local_player>(); }

            const std::unique_ptr<minecraft> mc{ mc_proxy->get() };
            if (!mc->get_instance()) { return std::make_unique<local_player>(); }

            const auto player_proxy{ mc->get_field(player_field) };
            if (!player_proxy) { return std::make_unique<local_player>(); }
            return player_proxy->get();
        }
        catch (...) { return std::make_unique<local_player>(); }
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
        @brief Asks the JVM the three questions mapping::decide needs answered.
        @details
        Separate from detect_mapping because the ANSWERS are worth reporting on
        their own: `mapping.detected` puts them on the wire, so a user whose
        client came back `unknown` can see which probe failed rather than being
        told only that it did.  Detection would otherwise be the one decision in
        chatwire with no evidence attached to it.
    */
    [[nodiscard]] inline auto probe_mapping() noexcept -> chatwire::mapping::probe_result
    {
        namespace map = chatwire::mapping;
        map::probe_result probe{};
        try
        {
            vmhook::hotspot::klass* const mcp{ vmhook::find_class(map::minecraft.clazz.mcp) };
            probe.mcp_class_present = mcp != nullptr;
            if (mcp)
            {
                probe.mcp_field_present =
                    vmhook::find_field(mcp, map::minecraft.the_minecraft.mcp).has_value();
                probe.srg_field_present =
                    vmhook::find_field(mcp, map::minecraft.the_minecraft.srg).has_value();
            }
            else
            {
                probe.obf_class_present = vmhook::find_class(map::minecraft.clazz.obf) != nullptr;
            }
        }
        catch (...) { return map::probe_result{}; }
        return probe;
    }

    /*
        @brief Probes the JVM and decides the mapping mode.
        @return the detected mode; mapping::mode::unknown means "not a supported
                Minecraft 1.8.9", which the caller must treat as do-not-inject.
    */
    [[nodiscard]] inline auto detect_mapping() noexcept -> chatwire::mapping::mode
    {
        return chatwire::mapping::decide(chatwire::sdk::probe_mapping());
    }

    /*
        @brief What a name in the table turned out to be in the attached JVM.
        @details
        `both` is not a curiosity, it is the common case on a vanilla client:
        obfuscation reuses one letter across kinds, so GuiNewChat has a field
        called `a` AND a method called `a`, and `printChatMessage` resolves to
        that letter.  Reporting whichever was checked first would have said
        "field" for a method, on the one mapping where a reader is least able to
        check for themselves.
    */
    enum class member_kind : std::uint8_t
    {
        /* The JVM has nothing of that name here.  A wrong table entry. */
        absent,
        field,
        method,
        both,
    };

    [[nodiscard]] constexpr auto member_kind_name(const member_kind k) noexcept
        -> std::string_view
    {
        switch (k)
        {
        case member_kind::field:  return "field";
        case member_kind::method: return "method";
        case member_kind::both:   return "field and method";
        case member_kind::absent: break;
        }
        return "absent";
    }

    /* @brief Whether the attached JVM has loaded a class of this name. */
    [[nodiscard]] inline auto class_exists(const std::string& class_name) noexcept -> bool
    {
        try
        {
            return !class_name.empty() && vmhook::find_class(class_name) != nullptr;
        }
        catch (...) { return false; }
    }

    /*
        @brief Whether `class_name` has a member spelled `member`, and which kind.
        @details
        BOTH lookups always run, never one and then the other conditionally: a
        name can be a field and a method at the same time -- under OBF it usually
        is -- and stopping at the first hit would report a method as a field.
        The field side is find_field, which walks the superclass chain and
        caches; the method side walks the same chain over each class's method
        array, which the field cache does not cover.

        NAME ONLY, no descriptor.  The question this answers is "is this table
        entry a real name in this build" -- the one the `avq` mistake got wrong,
        where a plausible name resolved to nothing at all.  Whether the right
        OVERLOAD exists is a different question, and the one place it matters
        (loadWorld) already pairs its name with a descriptor at the hook site.

        Metaspace reads only: nothing is called and no oop is touched, so this
        needs no thread state and no gate.  It is still not free -- find_class
        walks the ClassLoaderDataGraph on a miss -- so it belongs on a request,
        not in a loop.
    */
    [[nodiscard]] inline auto find_member(const std::string& class_name,
                                          const std::string& member) noexcept -> member_kind
    {
        try
        {
            if (class_name.empty() || member.empty()) { return member_kind::absent; }
            vmhook::hotspot::klass* const start{ vmhook::find_class(class_name) };
            if (!start) { return member_kind::absent; }

            const bool as_field{ vmhook::find_field(start, member).has_value() };

            bool as_method{ false };
            for (vmhook::hotspot::klass* k{ start }; k != nullptr && !as_method; k = k->get_super())
            {
                const std::int32_t count{ k->get_methods_count() };
                vmhook::hotspot::method** const methods{ k->get_methods_ptr() };
                if (!methods) { continue; }
                for (std::int32_t i{ 0 }; i < count; ++i)
                {
                    if (methods[i] && methods[i]->get_name() == member)
                    {
                        as_method = true;
                        break;
                    }
                }
            }

            if (as_field && as_method) { return member_kind::both; }
            if (as_field)              { return member_kind::field; }
            if (as_method)             { return member_kind::method; }
            return member_kind::absent;
        }
        catch (...) { return member_kind::absent; }
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

        const bool mc{ reg("Minecraft", map::minecraft.clazz,
                           static_cast<d::minecraft*>(nullptr)) };
        const bool player{ reg("EntityPlayerSP", map::entity_player_sp.clazz,
                               static_cast<d::local_player*>(nullptr)) };
        const bool component{ reg("IChatComponent", map::i_chat_component.clazz,
                                  static_cast<d::chat_component*>(nullptr)) };
        const bool chat_gui{ reg("GuiNewChat", map::gui_new_chat.clazz,
                                 static_cast<d::gui_new_chat*>(nullptr)) };
        const bool text{ reg("ChatComponentText", map::chat_component_text.clazz,
                             static_cast<d::chat_component_text*>(nullptr)) };
        const bool food{ reg("FoodStats", map::food_stats.clazz,
                             static_cast<d::food_stats*>(nullptr)) };

        if (!mc || !player)
        {
            chatwire::log::error("essential Minecraft classes missing; chatwire cannot run");
            return false;
        }
        if (!component) { chatwire::log::warn("IChatComponent missing; chat text may be empty"); }
        if (!chat_gui)  { chatwire::log::warn("GuiNewChat missing; incoming chat not observed"); }
        if (!text)      { chatwire::log::warn("ChatComponentText missing; client-side chat unavailable"); }
        if (!food)      { chatwire::log::warn("FoodStats missing; hunger will read as zero"); }
        return true;
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
            const auto class_name{ map::resolve(map::gui_new_chat.clazz) };
            const auto method{ map::resolve(map::gui_new_chat.print_chat_message) };
            const auto component{ map::resolve(map::i_chat_component.clazz) };
            if (class_name.empty() || method.empty() || component.empty()) { return false; }

            d::g_chat_callback.store(on_chat, std::memory_order_release);

            const std::string descriptor{ std::format("(L{};)V", component) };
            if (!vmhook::hook<d::gui_new_chat>(method, descriptor, &d::on_print_chat_message))
            {
                return false;
            }
            chatwire::log::info("chat observer installed on {}.{}{}",
                                class_name, method, descriptor);
            return true;
        }
        catch (...) { return false; }
    }

    /*
        @brief Called when the client changes world.  `loaded` false means it is
               LEAVING one.
        @details
        Runs INSIDE the loadWorld detour, on whichever thread is changing world.
        Must be quick and must not throw.
    */
    using world_callback = void (*)(bool loaded);

    /*
        @brief Hooks Minecraft.loadWorld so `on_world` sees every world change.
        @details
        Joins, respawns, server switches and disconnects all pass through this
        one method, and it is the only one that reports a disconnect POSITIVELY:
        every other route to that fact is polling `theWorld` and noticing it has
        become null.

        WHICH OVERLOAD.  1.8.9 has two, `loadWorld(WorldClient)` and
        `loadWorld(WorldClient, String)`, and the first is a one-line delegation
        to the second — so hooking the TWO-argument form catches both, while
        hooking the one-argument form would miss every caller that passes a
        loading message.  The longer one is therefore tried first and the shorter
        one only as a fallback, for a build where the pair has been patched into
        something else.

        The fallback carries its own NAME as well as its own descriptor: SRG
        calls the two overloads func_71353_a and func_71403_a, so reusing the
        first name with the second descriptor would be a lookup that cannot
        succeed.  See mapping::minecraft.load_world_short.

        Under OBF both overloads are called `a`, along with a great many
        unrelated methods on Minecraft, so the descriptor is not optional there:
        a name-only hook would install on whichever `a` came first in the class's
        method array.  That is why this refuses to install at all when the
        descriptor cannot be built.

        The detour observes and never cancels: suppressing loadWorld would leave
        the client with the world it was leaving and no way back.
    */
    [[nodiscard]] inline auto install_world_observer(const world_callback on_world) noexcept
        -> bool
    {
        namespace map = chatwire::mapping;
        namespace d   = chatwire::sdk::detail;
        try
        {
            const auto class_name{ map::resolve(map::minecraft.clazz) };
            const auto world{ d::world_client_descriptor() };
            if (class_name.empty() || world.empty()) { return false; }

            // NAME AND DESCRIPTOR TOGETHER, because under SRG the two overloads
            // do not share a name -- func_71353_a takes the loading message,
            // func_71403_a does not.  Pairing them is what keeps the fallback
            // from being a lookup that cannot succeed on two mappings out of
            // three.  The long form is first: the short one delegates to it.
            const std::pair<std::string, std::string> candidates[]{
                { map::resolve(map::minecraft.load_world),
                  std::format("({}Ljava/lang/String;)V", world) },
                { map::resolve(map::minecraft.load_world_short),
                  std::format("({})V", world) },
            };
            if (candidates[0].first.empty()) { return false; }

            d::g_world_callback.store(on_world, std::memory_order_release);

            for (const auto& [method, descriptor] : candidates)
            {
                if (method.empty()) { continue; }
                if (vmhook::hook<d::minecraft>(method, descriptor, &d::on_load_world))
                {
                    chatwire::log::info("world observer installed on {}.{}{}",
                                        class_name, method, descriptor);
                    return true;
                }
            }

            // Nothing was installed, so nothing may be left pointing at a
            // callback the caller will assume is unreachable.
            d::g_world_callback.store(nullptr, std::memory_order_release);
            return false;
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
            const auto class_name{ map::resolve(map::entity_player_sp.clazz) };
            const auto method{ map::resolve(map::entity_player_sp.send_chat_message) };
            if (class_name.empty() || method.empty()) { return false; }

            d::g_command_callback.store(on_typed, std::memory_order_release);

            constexpr std::string_view descriptor{ "(Ljava/lang/String;)V" };
            if (!vmhook::hook<d::local_player>(method, descriptor, &d::on_send_chat_message))
            {
                return false;
            }
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
            const auto method{ map::resolve(map::entity_player_sp.send_chat_message) };
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

            const auto p{ chatwire::sdk::detail::player() };
            if (!p->get_instance()) { return false; }
            const auto proxy{ p->get_method(method) };
            if (!proxy) { return false; }
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
            const auto class_name{ map::resolve(map::chat_component_text.clazz) };
            const auto method{ map::resolve(map::entity_player_sp.add_chat_message) };
            if (class_name.empty() || method.empty()) { return false; }
            vmhook::hotspot::klass* const k{ vmhook::find_class(class_name) };
            if (!k) { return false; }

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

                const std::string descriptor{
                    std::format("(L{};)V", map::resolve(map::i_chat_component.clazz)) };
                const bool added{ vmhook::jni_call_void(
                    player, method.c_str(), descriptor.c_str(), { component }) };
                vmhook::jni_release(component);
                vmhook::jni_release(player);
                return added;
            }

            // INSIDE A HOOK: the pure VMStructs path.  make_unique allocates the
            // object AND runs its real <init> -- which is what `new` compiles to
            // in Java, and which used to be twenty lines of scanning the class's
            // methods for the right constructor here.  Both steps happen with the
            // gate held, because the object is UNROOTED between them.
            const vmhook::java_thread_scope java{};
            if (!java) { return false; }

            const auto component{ vmhook::make_unique<d::chat_component_text>(text) };
            if (!component->get_instance()) { return false; }

            const auto p{ d::player() };
            if (!p->get_instance()) { return false; }
            const auto proxy{ p->get_method(method) };
            if (!proxy) { return false; }
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

            const auto mc_class{ map::resolve(map::minecraft.clazz) };
            const auto mc_field{ map::resolve(map::minecraft.the_minecraft) };
            const auto world_field{ map::resolve(map::minecraft.the_world) };
            const auto list_field{ map::resolve(map::world.player_entities) };
            const auto name_method{ map::resolve(map::entity.get_name) };
            const auto uuid_method{ map::resolve(map::entity.get_unique_id) };
            // theWorld's declared type, ASKED FOR rather than spelled out, so a
            // repackaged client answers for itself.  This used to build the
            // descriptor from a table entry that had no OBF name, which on a
            // vanilla client produced "Lnet/minecraft/client/multiplayer/
            // WorldClient;" -- a field lookup that matches nothing, and a player
            // list that came back empty every time.
            const auto world_type{ chatwire::sdk::detail::world_client_descriptor() };
            if (mc_class.empty() || mc_field.empty() || world_field.empty()
                || list_field.empty() || name_method.empty() || uuid_method.empty()
                || world_type.empty())
            {
                return out;
            }

            vmhook::hotspot::klass* const k{ vmhook::find_class(mc_class) };
            if (!k) { return out; }

            void* const mc{ vmhook::jni_static_object(k, mc_field.c_str(),
                                                      std::format("L{};", mc_class).c_str()) };
            if (!mc) { return out; }

            void* const world{ vmhook::jni_object_field(mc, world_field.c_str(),
                                                        world_type.c_str()) };
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

    /* @brief What `Minecraft.thePlayer` is, right now. */
    struct player_state
    {
        std::string  name{};
        /* Minecraft's own types, not widened: a client comparing this to a
           position it got from a packet wants the same number back. */
        double       x{ 0.0 };
        double       y{ 0.0 };
        double       z{ 0.0 };
        float        yaw{ 0.0F };
        float        pitch{ 0.0F };
        bool         on_ground{ false };
        std::int32_t dimension{ 0 };
        float        health{ 0.0F };
        std::int32_t food{ 0 };
        float        saturation{ 0.0F };
        std::int32_t experience_level{ 0 };
    };

    /*
        @brief The local player's state, or nullopt when not in a world.
        @details
        WRITTEN THE WAY vmhook DOCUMENTS, which is worth saying because most of
        this file was not: one `java_thread_scope`, then `get_field` and
        `get_method` on wrappers.  No oop is named, no reference is pinned and
        nothing has to be released.

        ONE SCOPE around resolve AND read, for the reason the scope's own
        documentation gives: inside it no collection can begin, so the player
        found at the top is still at the same address at the bottom.  Split into
        two scopes, a collection between them could move it.

        Everything here is a field load except `getHealth()`.  1.8.9 keeps health
        in the DataWatcher rather than in a member, so it is the one value that
        has to be called for -- which is also why this needs a scope at all
        rather than being the plain reads `player()` does.

        SHORT, as the scope requires: eleven loads and one call, no allocation,
        no lock, nothing that can block.  The whole JVM is queued behind it.
    */
    [[nodiscard]] inline auto local_player_state() noexcept -> std::optional<player_state>
    {
        namespace map = chatwire::mapping;
        namespace d   = chatwire::sdk::detail;
        try
        {
            const vmhook::java_thread_scope java{};
            if (!java) { return std::nullopt; }

            const auto player{ d::player() };
            if (!player->get_instance()) { return std::nullopt; }

            player_state out{};

            // A field that will not resolve leaves its member at the default
            // rather than failing the whole snapshot: a build missing one name
            // should still answer with the ten it has, and `mapping.verify` is
            // where a missing name gets reported as such.
            const auto load{ [&](const map::name& n, auto& into) noexcept
            {
                const auto spelling{ map::resolve(n) };
                if (spelling.empty()) { return; }
                if (const auto field{ player->get_field(spelling) }) { into = field->get(); }
            } };

            load(map::entity.pos_x, out.x);
            load(map::entity.pos_y, out.y);
            load(map::entity.pos_z, out.z);
            load(map::entity.rotation_yaw, out.yaw);
            load(map::entity.rotation_pitch, out.pitch);
            load(map::entity.on_ground, out.on_ground);
            load(map::entity.dimension, out.dimension);
            load(map::entity_player.experience_level, out.experience_level);

            if (const auto name_method{ player->get_method(map::resolve(map::entity.get_name)) })
            {
                out.name = name_method->call();
            }
            if (const auto health{ player->get_method(map::resolve(
                    map::entity_living_base.get_health)) })
            {
                out.health = health->call();
            }

            // foodStats is an object, so its two numbers are one indirection
            // further in -- and still inside the same scope, which is what makes
            // reading through it sound.
            if (const auto stats_field{ player->get_field(
                    map::resolve(map::entity_player.food_stats)) })
            {
                const std::unique_ptr<d::food_stats> stats{ stats_field->get() };
                if (stats->get_instance())
                {
                    if (const auto f{ stats->get_field(map::resolve(map::food_stats.food_level)) })
                    {
                        out.food = f->get();
                    }
                    if (const auto s{ stats->get_field(
                            map::resolve(map::food_stats.food_saturation_level)) })
                    {
                        out.saturation = s->get();
                    }
                }
            }

            return out;
        }
        catch (...) { return std::nullopt; }
    }

    /*
        @brief The class of the GUI currently open, "" when the world is showing.
        @details
        Minecraft.currentScreen is null whenever the player is looking at the
        world, so "" is the in-game answer rather than a failure.

        The DECLARED type is GuiScreen and that is useless to a caller -- every
        screen is one.  What is returned is the class the object REALLY is, read
        from its own klass, so a client sees `net/minecraft/client/gui/GuiChat`
        or, on a vanilla client, `axz`.  Which is the honest answer either way:
        it is what that jar calls it.

        A read, so no scope: resolving a field and following it is a load from an
        address, and a collection moving the screen changes where it is without
        changing what class it is.
    */
    [[nodiscard]] inline auto current_screen_class() noexcept -> std::string
    {
        namespace map = chatwire::mapping;
        namespace d   = chatwire::sdk::detail;
        try
        {
            const auto mc_field{ map::resolve(map::minecraft.the_minecraft) };
            const auto screen_field{ map::resolve(map::minecraft.current_screen) };
            if (mc_field.empty() || screen_field.empty()) { return {}; }

            const auto mc_proxy{ d::minecraft{}.get_field(mc_field) };
            if (!mc_proxy) { return {}; }
            const std::unique_ptr<d::minecraft> mc{ mc_proxy->get() };
            if (!mc->get_instance()) { return {}; }

            const auto screen_proxy{ mc->get_field(screen_field) };
            if (!screen_proxy) { return {}; }
            const std::unique_ptr<d::gui_screen> screen{ screen_proxy->get() };
            if (!screen->get_instance()) { return {}; }        // in the world

            vmhook::hotspot::klass* const k{ vmhook::klass_from_oop(screen->get_instance()) };
            if (!k) { return {}; }
            const vmhook::hotspot::symbol* const named{ k->get_name() };
            return named ? named->to_string() : std::string{};
        }
        catch (...) { return {}; }
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
        return chatwire::sdk::detail::player()->get_instance() != nullptr;
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
            const auto component{ map::resolve(map::chat_component_text.clazz) };
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
        // Same reasoning once more: a detour that is already running finds no
        // callback and reports nothing, rather than reaching a sink whose
        // server is on its way down.
        d::g_world_callback.store(nullptr, std::memory_order_release);
        // vmhook owns hook lifetime: shutdown_hooks() writes every patched entry
        // back and stops the watchdog that keeps them installed.  Explicit rather
        // than destructor-driven for the reason above -- unhooking touches the
        // JVM, and static destruction is not a time to do that.
        try { vmhook::shutdown_hooks(); } catch (...) { }
    }
}
