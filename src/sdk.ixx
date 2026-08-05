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
module;

// The shared preamble, FIRST and identical in every module.  See the header
// for why GCC 15 requires that of a modular build.
#include "core/prelude.hpp"

#include <vmhook/vmhook.hpp>

export module chatwire.sdk;

import chatwire.mapping;
import chatwire.core.log;

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
            @brief Calls a no-arg String-returning method, degrading to "".
            @details
            Every failure — method missing under this mapping, receiver stale,
            call refused — has to produce a string, because the caller is a
            detour that must not throw, and an unreadable chat line is not worth
            crashing a game over.
        */
        [[nodiscard]] auto text_via(const std::string& method) const noexcept -> std::string
        {
            try
            {
                if (method.empty() || !this->get_instance()) { return {}; }
                auto proxy{ const_cast<chat_component*>(this)->get_method(method.c_str()) };
                if (!proxy.has_value()) { return {}; }
                const auto value{ proxy->call() };
                if (value.threw()) { return {}; }
                return value.as_string();
            }
            catch (...) { return {}; }
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
        The pump hook needs its OWN wrapper type even though it targets the same
        Minecraft class as `minecraft` above: vmhook keys a hook's target class
        off the wrapper type, so reusing the accessor wrapper would tie the two
        together.
    */
    class pump_target : public vmhook::object<pump_target>
    {
    public:
        explicit pump_target(const vmhook::oop_t oop = nullptr) noexcept
            : vmhook::object<pump_target>{ oop }
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

    /* The callbacks the facade installs.  Plain function pointers: no captures
       to dangle, nothing to destroy at exit, and atomic so the detour thread
       and the installer never race. */
    inline std::atomic<void (*)(const char*, const char*)> g_chat_callback{ nullptr };
    inline std::atomic<void (*)()>                         g_tick_callback{ nullptr };

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

export namespace chatwire::sdk
{
    /*
        @brief Called for each chat line: (formatted, plain), both UTF-8.
        @details
        A plain function pointer, not std::function: it is installed once and
        never changes, and a function pointer cannot throw on copy or dangle
        after a lambda's captures die.  It runs ON THE GAME THREAD, inside a
        detour, so it must be quick and must not throw.
    */
    using chat_callback = void (*)(const char* formatted, const char* plain);

    /* @brief Called once per client tick, ON THE GAME THREAD, in a detour. */
    using tick_callback = void (*)();

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
        const bool pump{ reg("Minecraft (pump target)", map::minecraft::clazz,
                             static_cast<d::pump_target*>(nullptr)) };

        if (!mc || !player || !pump)
        {
            chatwire::log::error("essential Minecraft classes missing; chatwire cannot run");
            return false;
        }
        if (!component) { chatwire::log::warn("IChatComponent missing; chat text may be empty"); }
        if (!chat_gui)  { chatwire::log::warn("GuiNewChat missing; incoming chat not observed"); }
        return true;
    }

    /*
        @brief Hooks Minecraft.runTick so `on_tick` runs on the game thread.
        @details
        runTick is the hottest method in the client and certainly JIT-compiled,
        so it is deoptimised back to the interpreter first or the i2i detour
        never fires.  vmhook holds NO_COMPILE on a hooked Method, so the route
        stays put once established.
    */
    [[nodiscard]] inline auto install_pump(const tick_callback on_tick) noexcept -> bool
    {
        namespace map = chatwire::mapping;
        namespace d   = chatwire::sdk::detail;
        try
        {
            const auto class_name{ map::resolve(map::minecraft::clazz) };
            const auto method{ map::resolve(map::minecraft::run_tick) };
            if (class_name.empty() || method.empty()) { return false; }

            d::g_tick_callback.store(on_tick, std::memory_order_release);

            (void)vmhook::deoptimize_methods_if(
                [&class_name, &method](const std::string& cn, vmhook::hotspot::method* m)
                {
                    return cn == class_name && m && m->get_name() == method;
                });

            auto handle{ vmhook::scoped_hook<d::pump_target>(
                method, "()V",
                [](vmhook::return_value&, vmhook::borrowed<d::pump_target>) noexcept
                {
                    const auto cb{ d::g_tick_callback.load(std::memory_order_acquire) };
                    if (cb) { cb(); }
                }) };

            if (!handle.installed()) { return false; }
            d::hooks().push_back(std::move(handle));
            chatwire::log::info("pump installed on {}.{}", class_name, method);
            return true;
        }
        catch (...) { return false; }
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

            (void)vmhook::deoptimize_methods_if(
                [&class_name, &method](const std::string& cn, vmhook::hotspot::method* m)
                {
                    return cn == class_name && m && m->get_name() == method;
                });

            const std::string descriptor{ "(L" + component + ";)V" };
            auto handle{ vmhook::scoped_hook<d::gui_new_chat>(
                method, descriptor,
                [](vmhook::return_value&,
                   vmhook::borrowed<d::gui_new_chat>,
                   vmhook::borrowed<d::chat_component> line) noexcept
                {
                    // ON THE GAME THREAD, inside a detour.  Fully guarded: this
                    // frame's caller is Minecraft's interpreter, which has no
                    // handler for a C++ exception.
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
        @brief sendChatMessage(String) — goes to the SERVER.  GAME THREAD ONLY.
        @details
        Exactly as if the player typed it, so a leading '/' runs a command.
        Re-resolves the player every call rather than caching it: the local
        player is replaced on world change and on respawn, and a cached handle
        would be a stale oop by the time a queued task ran.
    */
    [[nodiscard]] inline auto send_chat(const std::string& text) noexcept -> bool
    {
        namespace map = chatwire::mapping;
        try
        {
            auto p{ chatwire::sdk::detail::player() };
            if (!p) { return false; }
            const auto method{ map::resolve(map::entity_player_sp::send_chat_message) };
            if (method.empty()) { return false; }
            auto proxy{ p->get_method(method.c_str()) };
            if (!proxy.has_value()) { return false; }
            return !proxy->call(text).threw();
        }
        catch (...) { return false; }
    }

    /*
        @brief addChatMessage(IChatComponent) — CLIENT-side.  GAME THREAD ONLY.
        @details
        Never transmitted; only this player sees it.  Builds a ChatComponentText
        from `text` first — allocate, then run <init> on the raw object, which is
        what `new` compiles to in Java.  The two steps stay adjacent because the
        object is UNROOTED between them: anything that could trigger a collection
        in the gap would move it out from under the constructor call.
    */
    [[nodiscard]] inline auto add_chat(const std::string& text) noexcept -> bool
    {
        namespace map = chatwire::mapping;
        namespace d   = chatwire::sdk::detail;
        try
        {
            const auto class_name{ map::resolve(map::chat_component_text::clazz) };
            if (class_name.empty()) { return false; }
            vmhook::hotspot::klass* const k{ vmhook::find_class(class_name) };
            if (!k) { return false; }

            auto component{ vmhook::new_object<d::chat_component>(k, k->get_instance_size()) };
            if (!component) { return false; }

            // <init> is spelled <init> under every mapping — the JVM reserves
            // the name, so no remapper touches it.  That is why this one lookup
            // needs no mapping entry.
            const std::string ctor_descriptor{ "(Ljava/lang/String;)V" };
            vmhook::hotspot::method** const methods{ k->get_methods_ptr() };
            const std::int32_t count{ k->get_methods_count() };
            if (!methods || count <= 0) { return false; }

            bool constructed{ false };
            for (std::int32_t i{ 0 }; i < count; ++i)
            {
                vmhook::hotspot::method* const m{ methods[i] };
                if (!m || !vmhook::hotspot::is_valid_pointer(m)) { continue; }
                if (std::string{ m->get_name() } != "<init>") { continue; }
                if (std::string{ m->get_signature() } != ctor_descriptor) { continue; }
                const vmhook::method_proxy ctor{ component.raw_unsafe(), m, ctor_descriptor };
                if (ctor.call(text).threw()) { return false; }
                constructed = true;
                break;
            }
            if (!constructed) { return false; }

            auto p{ d::player() };
            if (!p) { return false; }
            const auto method{ map::resolve(map::entity_player_sp::add_chat_message) };
            if (method.empty()) { return false; }
            auto proxy{ p->get_method(method.c_str()) };
            if (!proxy.has_value()) { return false; }
            return !proxy->call(component).threw();
        }
        catch (...) { return false; }
    }

    /* @brief True when the local player exists, i.e. we are in a world. */
    [[nodiscard]] inline auto in_world() noexcept -> bool
    {
        return static_cast<bool>(chatwire::sdk::detail::player());
    }

    /*
        @brief Removes every installed hook.  GAME THREAD ONLY.
        @details
        Explicit rather than destructor-driven: unhooking touches the JVM, and
        doing that from static destruction or DLL unload would reach a VM that
        may already be tearing down.
    */
    inline auto remove_hooks() noexcept -> void
    {
        namespace d = chatwire::sdk::detail;
        d::g_chat_callback.store(nullptr, std::memory_order_release);
        d::g_tick_callback.store(nullptr, std::memory_order_release);
        try { d::hooks().clear(); } catch (...) { }
    }
}
