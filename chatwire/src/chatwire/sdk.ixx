module;

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

export module chatwire.sdk;
import chatwire.log;
import chatwire.mapping;
import vmhook;

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

// THE ONE PLACE vmhook IS INCLUDED.  See the header comment above for why
// that boundary matters.

export namespace chatwire::sdk::detail
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
        net.minecraft.client.multiplayer.WorldClient — the argument of loadWorld,
        and the world `playerEntities` is read off.

        It used to be the one wrapper here that was never register_class'd,
        because a detour argument declared as std::unique_ptr<W> is wrapped
        directly around the incoming oop and the world was read for one thing:
        whether it was null.  It is registered now, because registration is what
        buys FIELD lookups and `players()` reads `playerEntities` through this
        wrapper rather than through a raw reference.  The field is declared on
        World and inherited, which the lookup walks to find.
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
        net.minecraft.entity.player.EntityPlayer — an entry of `playerEntities`.
        Separate from local_player, which is EntityPlayerSP: the list holds every
        player the client has loaded, and only one of them is this one.
    */
    class player_entity : public vmhook::object<player_entity>
    {
    public:
        explicit player_entity(const vmhook::oop_t oop = nullptr) noexcept
            : vmhook::object<player_entity>{ oop }
        {
        }
    };

    /*
        java.util.Collection — every list and set chatwire reads: playerEntities,
        loadedEntityList, the tab list, the teams, a team's members, an
        objective's scores.  Wrapped only so a field or a return value can be
        held as an object; the WALKING is done by vmhook::collection, which knows
        how to iterate a java.util.Collection whatever its implementation.

        Deliberately one wrapper for all of them.  A wrapper buys field and
        method lookups BY NAME, and nothing here looks anything up on a
        collection -- it hands the object straight to vmhook.
    */
    class java_collection : public vmhook::object<java_collection>
    {
    public:
        explicit java_collection(const vmhook::oop_t oop = nullptr) noexcept
            : vmhook::object<java_collection>{ oop }
        {
        }
    };

    /*
        java.lang.String — needed only so `collection::to_vector` has an element
        type for a Collection<String>; a team's members are one.  Nothing is
        looked up on it: the text comes from vmhook::read_java_string, which
        decodes the object directly.
    */
    class java_string : public vmhook::object<java_string>
    {
    public:
        explicit java_string(const vmhook::oop_t oop = nullptr) noexcept
            : vmhook::object<java_string>{ oop } { }
    };

    /* net.minecraft.scoreboard.Scoreboard — objectives, scores and teams. */
    class scoreboard : public vmhook::object<scoreboard>
    {
    public:
        explicit scoreboard(const vmhook::oop_t oop = nullptr) noexcept
            : vmhook::object<scoreboard>{ oop } { }
    };

    /* net.minecraft.scoreboard.ScoreObjective — one scoreboard column. */
    class score_objective : public vmhook::object<score_objective>
    {
    public:
        explicit score_objective(const vmhook::oop_t oop = nullptr) noexcept
            : vmhook::object<score_objective>{ oop } { }
    };

    /* net.minecraft.scoreboard.Score — one row of one objective. */
    class score : public vmhook::object<score>
    {
    public:
        explicit score(const vmhook::oop_t oop = nullptr) noexcept
            : vmhook::object<score>{ oop } { }
    };

    /* net.minecraft.scoreboard.ScorePlayerTeam — a team and its nametag parts. */
    class score_player_team : public vmhook::object<score_player_team>
    {
    public:
        explicit score_player_team(const vmhook::oop_t oop = nullptr) noexcept
            : vmhook::object<score_player_team>{ oop } { }
    };

    /* net.minecraft.client.network.NetHandlerPlayClient — the connection. */
    class net_handler : public vmhook::object<net_handler>
    {
    public:
        explicit net_handler(const vmhook::oop_t oop = nullptr) noexcept
            : vmhook::object<net_handler>{ oop } { }
    };

    /* net.minecraft.client.network.NetworkPlayerInfo — one tab-list row. */
    class player_info : public vmhook::object<player_info>
    {
    public:
        explicit player_info(const vmhook::oop_t oop = nullptr) noexcept
            : vmhook::object<player_info>{ oop } { }
    };

    /* com.mojang.authlib.GameProfile — the name and uuid behind a tab row. */
    class game_profile : public vmhook::object<game_profile>
    {
    public:
        explicit game_profile(const vmhook::oop_t oop = nullptr) noexcept
            : vmhook::object<game_profile>{ oop } { }
    };

    /* net.minecraft.entity.Entity — anything in loadedEntityList. */
    class any_entity : public vmhook::object<any_entity>
    {
    public:
        explicit any_entity(const vmhook::oop_t oop = nullptr) noexcept
            : vmhook::object<any_entity>{ oop } { }
    };

    /*
        @brief Every element of ANY java.util.Collection, as wrappers.
        @details
        `vmhook::collection::to_vector` has fast paths for ArrayList,
        LinkedList, HashSet, LinkedHashSet and TreeSet, plus a `get(int)`
        fallback -- which between them cover every collection chatwire reads
        except the two it needs most.  `Scoreboard.getTeams()` and
        `NetHandlerPlayClient.getPlayerInfoMap()` both return a HashMap VALUES
        VIEW: it is a Collection, `size()` answers correctly, and it is none of
        those classes and has no `get(int)`.  Both came back empty while
        reporting a size of one.

        So: the fast path first, and when it produces nothing for a collection
        that says it is not empty, walk the Iterator instead.  That is the one
        way to read a Collection that is true for all of them, it is plain
        `get_method` and `call` like everything else here, and the fallback
        condition is exactly "the fast path did not apply" rather than a guess
        about the class.

        Must be called inside a java_thread_scope: it is two calls per element.
    */
    template<typename element_type>
    [[nodiscard]] inline auto elements_of(const std::unique_ptr<java_collection>& held,
                                          const std::size_t limit) noexcept
        -> std::vector<std::unique_ptr<element_type>>
    {
        std::vector<std::unique_ptr<element_type>> out;
        try
        {
            if (!held->get_instance()) { return out; }

            const vmhook::collection view{ held->get_instance() };
            out = view.to_vector<element_type>();
            if (!out.empty() || view.size() <= 0) { return out; }

            const auto iterator_of{ held->get_method("iterator", "()Ljava/util/Iterator;") };
            if (!iterator_of) { return out; }
            const std::unique_ptr<java_collection> it{ iterator_of->call() };
            if (!it->get_instance()) { return out; }

            const auto has_next{ it->get_method("hasNext", "()Z") };
            const auto next{ it->get_method("next", "()Ljava/lang/Object;") };
            if (!has_next || !next) { return out; }

            while (out.size() < limit)
            {
                bool more{ false };
                more = has_next->call();
                if (!more) { break; }
                std::unique_ptr<element_type> element{ next->call() };
                if (!element || !element->get_instance()) { continue; }
                out.push_back(std::move(element));
            }
        }
        catch (...) { }
        return out;
    }

    /*
        @brief The class an object REALLY is, in this jar's spelling.
        @details
        `net/minecraft/entity/monster/EntityZombie` on a deobfuscated client,
        `zj` on a vanilla one.  Both are what that jar calls it, which is the
        only answer either client can give -- and `mapping.resolve` is there to
        relate the two.

        Reads the klass out of the object's header, so it needs no call and no
        gate; a collection moving the object changes where it is, never what it
        is.
    */
    [[nodiscard]] inline auto class_name_of(void* const instance) noexcept -> std::string
    {
        try
        {
            if (!instance) { return {}; }
            vmhook::hotspot::klass* const k{ vmhook::klass_from_oop(instance) };
            if (!k) { return {}; }
            const vmhook::hotspot::symbol* const named{ k->get_name() };
            return named ? named->to_string() : std::string{};
        }
        catch (...) { return {}; }
    }

    /*
        @brief `()Lsomething;` — a descriptor for a no-argument object getter.
        @details
        BUILT, never written.  The class in it is whatever this jar calls it, so
        a literal would be right on one mapping out of three.  Empty when the
        class has no name under the detected mapping, which callers treat as
        "cannot ask".
    */
    [[nodiscard]] inline auto returns(const map::name& n) -> std::string
    {
        const auto resolved{ map::resolve(n) };
        return resolved.empty() ? std::string{} : std::format("()L{};", resolved);
    }

    /* @brief The text of a no-argument String getter, or "". */
    template<typename wrapper_type>
    [[nodiscard]] inline auto string_call(const std::unique_ptr<wrapper_type>& on,
                                          const map::name& method) noexcept -> std::string
    {
        try
        {
            const auto spelling{ map::resolve(method) };
            if (spelling.empty() || !on->get_instance()) { return {}; }
            const auto proxy{ on->get_method(spelling, "()Ljava/lang/String;") };
            if (!proxy) { return {}; }

            // ASSIGNED, not brace-initialised.  `call()` returns a variant-backed
            // value with a TEMPLATED conversion operator, so `std::string{ v }`
            // is direct-initialisation and picks whichever instantiation the
            // constructor set accepts -- which was `char`, yielding a
            // one-character string holding NUL.  Every name read through here
            // came back as one NUL character and the scoreboard looked empty.  Assignment
            // asks for std::string specifically and cannot pick anything else.
            std::string out{};
            out = proxy->call();
            return out;
        }
        catch (...) { return {}; }
    }

    /* @brief An IChatComponent getter, flattened to its formatted text. */
    template<typename wrapper_type>
    [[nodiscard]] inline auto component_call(const std::unique_ptr<wrapper_type>& on,
                                             const map::name& method) noexcept -> std::string
    {
        try
        {
            const auto spelling{ map::resolve(method) };
            const auto descriptor{ returns(map::i_chat_component.clazz) };
            if (spelling.empty() || descriptor.empty() || !on->get_instance()) { return {}; }
            const auto proxy{ on->get_method(spelling, descriptor) };
            if (!proxy) { return {}; }
            const std::unique_ptr<chat_component> line{ proxy->call() };
            if (!line->get_instance()) { return {}; }
            return text_of(line, map::resolve(map::i_chat_component.get_formatted_text));
        }
        catch (...) { return {}; }
    }

    /*
        java.util.UUID — what getUniqueID() hands back, and the only thing done
        with it is toString().
    */
    class java_uuid : public vmhook::object<java_uuid>
    {
    public:
        explicit java_uuid(const vmhook::oop_t oop = nullptr) noexcept
            : vmhook::object<java_uuid>{ oop }
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

    /* net.minecraft.client.gui.GuiIngame - holds the tab overlay. */
    class gui_ingame : public vmhook::object<gui_ingame>
    {
    public:
        explicit gui_ingame(const vmhook::oop_t oop = nullptr) noexcept
            : vmhook::object<gui_ingame>{ oop } { }
    };

    /* net.minecraft.client.gui.GuiPlayerTabOverlay - the tab list as drawn. */
    class tab_overlay : public vmhook::object<tab_overlay>
    {
    public:
        explicit tab_overlay(const vmhook::oop_t oop = nullptr) noexcept
            : vmhook::object<tab_overlay>{ oop } { }
    };

    /* net.minecraft.scoreboard.Team - formatPlayerName's first argument. */
    class team_handle : public vmhook::object<team_handle>
    {
    public:
        explicit team_handle(const vmhook::oop_t oop = nullptr) noexcept
            : vmhook::object<team_handle>{ oop } { }
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
        The name rewriter.  Same shape as the other callbacks and for the same
        reasons: a plain function pointer, atomic, nothing to dangle.
    */
    inline std::atomic<bool (*)(const char*, std::string&)> g_rewrite_callback{ nullptr };

    /*
        @brief The ScorePlayerTeam.formatPlayerName detour.
        @details
        STATIC, so there is no receiver: the first declared parameter is the
        method's own first argument.  The Team may be null -- a player on no team
        still goes through here -- which is why it is declared and ignored rather
        than left out.

        THE ARGUMENT IS REWRITTEN, NOT THE RETURN.  set_arg replaces the raw name
        and lets Minecraft do the decorating, so a rewritten name still gets its
        team's colour, its prefix and its suffix exactly as the game would have
        applied them.  Forcing the return instead would mean reproducing that
        formatting here, and getting it subtly wrong for every server that uses
        a feature this file had not thought about.

        Runs on the render thread, many times a frame -- once per visible
        nametag and once per tab row.  So: no allocation unless a rule actually
        matches, no lock (the rule set is a shared_ptr swapped whole), and a
        blanket catch, because the frame above is Minecraft's interpreter.
    */
    inline auto on_format_player_name(vmhook::return_value& ret,
                                      const std::unique_ptr<team_handle>&,
                                      const std::string& player_name) noexcept
        -> void
    {
        try
        {
            const auto cb{ g_rewrite_callback.load(std::memory_order_acquire) };
            if (!cb || player_name.empty()) { return; }

            std::string replacement;
            if (cb(player_name.c_str(), replacement))
            {
                // Index 1: this method is static, so 0 is the Team and 1 is the
                // name.  On an instance method 0 would be the first argument
                // rather than the receiver -- see vmhook's own note on set_arg.
                ret.set_arg(1, replacement);
            }
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
        @brief The Minecraft singleton, or a wrapper whose instance is null.
        @details
        `Minecraft.theMinecraft` is a STATIC field, read through an
        instance-less wrapper exactly as an instance field would be.  Every root
        below starts here, which is why it is one function rather than four
        copies of the same two lines.
    */
    [[nodiscard]] inline auto client() noexcept -> std::unique_ptr<minecraft>
    {
        try
        {
            const auto field{ map::resolve(map::minecraft.the_minecraft) };
            if (field.empty()) { return std::make_unique<minecraft>(); }
            const auto proxy{ minecraft{}.get_field(field) };
            if (!proxy) { return std::make_unique<minecraft>(); }
            return proxy->get();
        }
        catch (...) { return std::make_unique<minecraft>(); }
    }

    /*
        @brief `Minecraft.theWorld`, or a null wrapper on the title screen.
        @details
        Not-in-a-world is the normal case rather than a failure, and every caller
        treats it as "nothing to report" -- so this returns a wrapper whose
        instance is null instead of an optional nobody would branch on
        differently.
    */
    [[nodiscard]] inline auto client_world() noexcept -> std::unique_ptr<world_client>
    {
        try
        {
            const auto field{ map::resolve(map::minecraft.the_world) };
            const auto mc{ client() };
            if (field.empty() || !mc->get_instance()) { return std::make_unique<world_client>(); }
            const auto proxy{ mc->get_field(field) };
            if (!proxy) { return std::make_unique<world_client>(); }
            return proxy->get();
        }
        catch (...) { return std::make_unique<world_client>(); }
    }

    /*
        @brief `World.getScoreboard()` — the CLIENT's copy.
        @details
        There is no other one here.  It holds exactly what the server has told
        this player, so an objective the server hides from you is hidden from
        this too, and a team it has not sent does not exist as far as chatwire
        is concerned.  That is a property of the game rather than a limitation of
        the bridge, and it is why the command is named after the getter.
    */
    [[nodiscard]] inline auto client_scoreboard() noexcept -> std::unique_ptr<scoreboard>
    {
        try
        {
            const auto world{ client_world() };
            const auto method{ map::resolve(map::world.get_scoreboard) };
            const auto descriptor{ returns(map::scoreboard.clazz) };
            if (!world->get_instance() || method.empty() || descriptor.empty())
            {
                return std::make_unique<scoreboard>();
            }
            const auto proxy{ world->get_method(method, descriptor) };
            if (!proxy) { return std::make_unique<scoreboard>(); }
            return proxy->call();
        }
        catch (...) { return std::make_unique<scoreboard>(); }
    }

    /*
        @brief `GuiPlayerTabOverlay.getPlayerName(info)` -- one whole tab row.
        @details
        Asked of the game rather than assembled here, because the rule it
        implements is not "the display name": with no server-set display name
        the row is the team's prefix, the profile name and the team's suffix,
        and any copy of that logic is a copy that goes stale.

        The overlay may not exist yet -- it is created with the in-game GUI --
        in which case this is "" and the caller falls back to display_name.

        Must be called inside a java_thread_scope; it is a field walk and a call.
    */
    [[nodiscard]] inline auto tab_line_for(const std::unique_ptr<player_info>& row) noexcept
        -> std::string
    {
        namespace map = chatwire::mapping;
        try
        {
            if (!row->get_instance()) { return {}; }

            const auto mc{ client() };
            if (!mc->get_instance()) { return {}; }
            const auto gui_proxy{ mc->get_field(map::resolve(map::minecraft.ingame_gui)) };
            if (!gui_proxy) { return {}; }
            const std::unique_ptr<gui_ingame> gui{ gui_proxy->get() };
            if (!gui->get_instance()) { return {}; }

            const auto overlay_proxy{ gui->get_field(
                map::resolve(map::gui_ingame.overlay_player_list)) };
            if (!overlay_proxy) { return {}; }
            const std::unique_ptr<tab_overlay> overlay{ overlay_proxy->get() };
            if (!overlay->get_instance()) { return {}; }

            const auto method{ overlay->get_method(
                map::resolve(map::gui_player_tab_overlay.get_player_name),
                std::format("(L{};)Ljava/lang/String;",
                            map::resolve(map::network_player_info.clazz))) };
            if (!method) { return {}; }
            std::string out{};
            out = method->call(row);
            return out;
        }
        catch (...) { return {}; }
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

export namespace chatwire::sdk
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
        const bool world_class{ reg("WorldClient", map::world_client.clazz,
                                    static_cast<d::world_client*>(nullptr)) };
        const bool other{ reg("EntityPlayer", map::entity_player.clazz,
                              static_cast<d::player_entity*>(nullptr)) };
        // Entity, for `loadedEntityList`.  Registration is what buys FIELD
        // lookups, and this is the wrapper posX/posY/posZ are read through.
        // Everything else added with it -- Scoreboard, teams, the tab list --
        // is reached by METHOD, which vmhook resolves from the object's own
        // class, so those wrappers need no name bound to them.
        const bool entities{ reg("Entity", map::entity.clazz,
                                 static_cast<d::any_entity*>(nullptr)) };
        // ScorePlayerTeam carries the hook, so it needs a class name bound to
        // it -- vmhook::hook resolves its target through the registration.
        const bool team_class{ reg("ScorePlayerTeam", map::score_player_team.clazz,
                                   static_cast<d::score_player_team*>(nullptr)) };
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
        if (!world_class) { chatwire::log::warn("WorldClient missing; playerEntities unavailable"); }
        if (!other)     { chatwire::log::warn("EntityPlayer missing; playerEntities unavailable"); }
        if (!entities)  { chatwire::log::warn("Entity missing; loadedEntityList unavailable"); }
        if (!team_class) { chatwire::log::warn("ScorePlayerTeam missing; name rewriting "
                                               "unavailable"); }
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
        // Cleared first among equals: a detour already running with no callback
        // leaves the name ALONE, which is the safe direction.  A half-detached
        // chatwire that kept rewriting what the player sees would be worse than
        // one that stopped.
        d::g_rewrite_callback.store(nullptr, std::memory_order_release);
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
        @brief Offered every name the game is about to decorate; true rewrites it.
        @details
        Runs INSIDE the formatPlayerName detour, on the render thread, several
        times a frame.  It must be quick, must not throw, must not allocate
        unless it is actually changing something, and must not take a lock any
        other thread holds for long.
    */
    using name_rewriter = bool (*)(const char* original, std::string& replacement);

    /*
        @brief Hooks ScorePlayerTeam.formatPlayerName so `on_name` can rewrite it.
        @details
        ONE method, and it is the one every decorated name in the game passes
        through: the nametag above a head, and a tab row whose display name the
        server has not overridden.  Hooking here rather than at the two call
        sites means a rewrite is consistent between them by construction.

        WHAT IT DOES NOT REACH, said plainly because a client will notice: a tab
        row for which the server HAS pushed a display name does not go through
        formatPlayerName at all -- Minecraft uses the component it was sent.  So
        a rewrite changes nametags always, and tab rows on servers that leave the
        name alone.  `getPlayerName` reports what a row really says, which is how
        a caller can tell which case it is in.

        The descriptor is built rather than written: it names Team and
        java.lang.String, and Team is spelled differently on each mapping.
    */
    [[nodiscard]] inline auto install_name_rewriter(const name_rewriter on_name) noexcept -> bool
    {
        namespace map = chatwire::mapping;
        namespace d   = chatwire::sdk::detail;
        try
        {
            const auto class_name{ map::resolve(map::score_player_team.clazz) };
            const auto method{ map::resolve(map::score_player_team.format_player_name) };
            const auto team{ map::resolve(map::team.clazz) };
            if (class_name.empty() || method.empty() || team.empty()) { return false; }

            d::g_rewrite_callback.store(on_name, std::memory_order_release);

            const std::string descriptor{
                std::format("(L{};Ljava/lang/String;)Ljava/lang/String;", team) };
            if (!vmhook::hook<d::score_player_team>(method, descriptor,
                                                    &d::on_format_player_name))
            {
                d::g_rewrite_callback.store(nullptr, std::memory_order_release);
                return false;
            }
            chatwire::log::info("name rewriter installed on {}.{}{}",
                                class_name, method, descriptor);
            return true;
        }
        catch (...) { return false; }
    }

    /* @brief The tab list's header and footer, as the player sees them. */
    struct tab_decoration
    {
        std::string header{};
        std::string footer{};
    };

    /*
        @brief `GuiPlayerTabOverlay.header` and `.footer`.
        @details
        These live nowhere else.  A packet pushes them straight into the overlay,
        so the roster commands cannot report them and a client that wants what
        the player is looking at has to come here.  Both are "" when the server
        has set none, which is the usual case on vanilla servers.
    */
    [[nodiscard]] inline auto tab_decorations() noexcept -> tab_decoration
    {
        namespace map = chatwire::mapping;
        namespace d   = chatwire::sdk::detail;
        tab_decoration out{};
        try
        {
            const vmhook::java_thread_scope java{};
            if (!java) { return out; }

            const auto mc{ d::client() };
            if (!mc->get_instance()) { return out; }

            const auto gui_proxy{ mc->get_field(map::resolve(map::minecraft.ingame_gui)) };
            if (!gui_proxy) { return out; }
            const std::unique_ptr<d::gui_ingame> gui{ gui_proxy->get() };
            if (!gui->get_instance()) { return out; }

            const auto overlay_proxy{ gui->get_field(
                map::resolve(map::gui_ingame.overlay_player_list)) };
            if (!overlay_proxy) { return out; }
            const std::unique_ptr<d::tab_overlay> overlay{ overlay_proxy->get() };
            if (!overlay->get_instance()) { return out; }

            const auto read{ [&](const map::name& field) noexcept -> std::string
            {
                const auto proxy{ overlay->get_field(map::resolve(field)) };
                if (!proxy) { return {}; }
                const std::unique_ptr<d::chat_component> line{ proxy->get() };
                if (!line->get_instance()) { return {}; }
                return d::text_of(line,
                                  map::resolve(map::i_chat_component.get_formatted_text));
            } };
            out.header = read(map::gui_player_tab_overlay.header);
            out.footer = read(map::gui_player_tab_overlay.footer);
        }
        catch (...) { }
        return out;
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

            // ONE PATH, and it is the one vmhook documents.  This used to
            // branch: a JNI bridge off a hook, the scope below inside one.  The
            // bridge existed because `method_proxy::call()` was broken on every
            // JDK before vmhook 6.0.0 -- it resolved the call stub through a
            // VMStructs entry no JVM publishes, so every Java call was a silent
            // no-op and the JNI fallback was the only thing that worked.  6.0.0
            // fixed the stub, and the branch became two ways to do one thing,
            // one of them undocumented and slated for deletion upstream.
            //
            // The scope is not the lesser half.  It ATTACHES this thread if it
            // is not already a JavaThread and then enters `_thread_in_Java`,
            // which it will not do while a stop-the-world collection is running
            // -- so no collection can begin inside it.  That is the property a
            // detour has for free, and it is why the old advice was "call from
            // inside a hook".  There is no separate attach_thread() call here
            // any more: the scope is the attach.
            //
            // RESOLVE AND CALL IN ONE SCOPE.  Split into two, a collection
            // between them could move the player found by the first.
            const vmhook::java_thread_scope java{};
            if (!java) { return false; }

            const auto p{ chatwire::sdk::detail::player() };
            if (!p->get_instance()) { return false; }

            // NAME AND DESCRIPTOR.  Under OBF this method is called `e`, and so
            // are a dozen unrelated methods on EntityPlayerSP -- a name-only
            // lookup takes whichever comes first in the class's method array.
            // The descriptor is not optional on the one mapping most users run.
            const auto proxy{ p->get_method(method, "(Ljava/lang/String;)V") };
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

            // ONE PATH, vmhook's documented one -- see the note in send_chat
            // for why the JNI branch that used to be here is gone.
            //
            // ALLOCATE AND CONSTRUCT AND CALL, ALL INSIDE THE SCOPE, and that is
            // the correctness property rather than tidiness.  A TLAB is a
            // lockless bump pointer, and between the allocation and <init> the
            // object is UNROOTED: a collection landing anywhere in allocate ->
            // construct -> call would move it out from under the next step.
            // make_unique does the first two as one -- it is what `new` compiles
            // to in Java -- and the scope holds all three together.
            const vmhook::java_thread_scope java{};
            if (!java) { return false; }

            const auto component{ vmhook::make_unique<d::chat_component_text>(text) };
            if (!component->get_instance()) { return false; }

            const auto p{ d::player() };
            if (!p->get_instance()) { return false; }

            // NAME AND DESCRIPTOR, and here it is not a precaution but the
            // difference between working and not.  Under OBF this is
            // EntityPlayerSP.a, which is also sendChatMessage's neighbour and
            // several other things; resolving by name alone found one of the
            // others and addChatMessage silently did nothing on every vanilla
            // client.  The descriptor names IChatComponent in whichever spelling
            // this jar uses, so it is built rather than written.
            const std::string descriptor{
                std::format("(L{};)V", map::resolve(map::i_chat_component.clazz)) };
            const auto proxy{ p->get_method(method, descriptor) };
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
        namespace d   = chatwire::sdk::detail;
        std::vector<player_identity> out;
        try
        {
            // Table lookups first, outside the scope: they touch no Java, and
            // every microsecond inside a scope is a microsecond the whole VM
            // spends waiting for this thread.
            const auto mc_field{ map::resolve(map::minecraft.the_minecraft) };
            const auto world_field{ map::resolve(map::minecraft.the_world) };
            const auto list_field{ map::resolve(map::world.player_entities) };
            const auto name_method{ map::resolve(map::entity.get_name) };
            const auto uuid_method{ map::resolve(map::entity.get_unique_id) };
            if (mc_field.empty() || world_field.empty() || list_field.empty()
                || name_method.empty() || uuid_method.empty())
            {
                return out;
            }

            // ONE SCOPE for the whole walk, which matters more here than
            // anywhere else in this file: it is the longest sequence of Java
            // work chatwire does, so it is where a collection would be likeliest
            // to land between resolving something and using it.  Inside the
            // scope none can begin.
            //
            // It is also the reason this is bounded below.  A scope is a
            // safepoint the VM waits on, and a thousand players would hold it
            // for long enough to stutter the game.
            const vmhook::java_thread_scope java{};
            if (!java) { return out; }

            const auto mc_proxy{ d::minecraft{}.get_field(mc_field) };
            if (!mc_proxy) { return out; }
            const std::unique_ptr<d::minecraft> mc{ mc_proxy->get() };
            if (!mc->get_instance()) { return out; }

            const auto world_proxy{ mc->get_field(world_field) };
            if (!world_proxy) { return out; }
            const std::unique_ptr<d::world_client> world{ world_proxy->get() };
            if (!world->get_instance()) { return out; }        // title screen

            const auto list_proxy{ world->get_field(list_field) };
            if (!list_proxy) { return out; }
            const std::unique_ptr<d::java_collection> held{ list_proxy->get() };
            if (!held->get_instance()) { return out; }

            // vmhook::list knows how to walk a java.util.Collection, so the
            // size()/get(i) loop this used to run by hand is one call.  What it
            // returns is wrappers, not addresses.
            const vmhook::list entities{ held->get_instance() };
            auto found{ entities.to_vector<d::player_entity>() };

            out.reserve(found.size());
            for (const auto& entity : found)
            {
                if (!entity || !entity->get_instance()) { continue; }

                player_identity who{};
                if (const auto named{ entity->get_method(name_method) })
                {
                    who.name = named->call();
                }
                if (const auto unique_id{ entity->get_method(uuid_method) })
                {
                    const std::unique_ptr<d::java_uuid> id{ unique_id->call() };
                    if (id->get_instance())
                    {
                        if (const auto as_text{ id->get_method("toString") })
                        {
                            who.uuid = as_text->call();
                        }
                    }
                }
                if (!who.name.empty() || !who.uuid.empty()) { out.push_back(std::move(who)); }
            }
        }
        catch (...) { }
        return out;
    }

    /*
        @brief What a rewrite template can put in a name, per player.
        @details
        A SNAPSHOT, refreshed off the render thread and read from inside a
        detour.  `{health}` in a nametag has to come from somewhere, and the one
        place it must not come from is the detour itself: that runs on the render
        thread several times a frame, and reading a player's health means walking
        the entity list and calling into Java.  Doing that per drawn name would
        turn a nametag into a stall.

        So the socket side takes this snapshot on a timer and the detour reads
        the pointer.  The cost is that a nametag can be up to one refresh stale,
        which for a health number a human is reading is not a cost at all.
    */
    struct player_snapshot
    {
        std::string name{};
        float       health{ 0.0F };
        std::int32_t food{ 0 };
        std::int32_t ping{ 0 };
        double      x{ 0.0 };
        double      y{ 0.0 };
        double      z{ 0.0 };
    };

    /* @brief One row of a scoreboard objective. */
    struct score_entry
    {
        /*
            The "player" this score belongs to, which on most servers is not a
            player: a sidebar is built out of fake entries whose NAMES are the
            text you see.  Reported as the game holds it rather than filtered,
            because deciding which entries are real is the caller's business.
        */
        std::string  name{};
        std::int32_t points{ 0 };
    };

    /* @brief An objective in one display slot, and what it currently shows. */
    struct objective_view
    {
        /* "list" (tab), "sidebar" or "belowName" — the slot that was asked for. */
        std::string             slot{};
        /* Empty when the server has put no objective in this slot. */
        std::string             name{};
        std::string             display_name{};
        std::vector<score_entry> scores{};
    };

    /*
        @brief One team, with the two strings that decorate its members' names.
        @details
        `prefix` and `suffix` are the COLOURED forms — what the game actually
        puts either side of a member's name, which is what makes one nametag red
        and another blue.  A client that wants the nametag a player is drawn with
        can build it as prefix + name + suffix without asking anything else.
    */
    struct team_view
    {
        std::string              name{};
        std::string              display_name{};
        std::string              prefix{};
        std::string              suffix{};
        std::vector<std::string> members{};
    };

    /*
        @brief One row of the TAB LIST, which is not the same as playerEntities.
        @details
        playerEntities is who the client has loaded as entities — the players
        near enough to exist.  This is everyone the SERVER says is connected,
        which on a large server is a different and much longer list.  Both are
        offered because both are real questions, and confusing them is the
        commonest mistake in this area.
    */
    struct tab_entry
    {
        std::string  name{};
        std::string  uuid{};
        /* Milliseconds, as the tab list's signal bars are drawn from. */
        std::int32_t ping{ 0 };
        /*
            The name the tab list DRAWS, which the server may have coloured or
            replaced.  Falls back to the profile name when the server set none,
            exactly as the game does.
        */
        std::string  display_name{};
        /*
            THE COMPLETE LINE, straight from the method that draws it --
            `GuiPlayerTabOverlay.getPlayerName`.  It is not always the same as
            `display_name`: when the server has pushed no display name the game
            builds the row out of the team's prefix, the profile name and the
            team's suffix, and only this reports the result.  Asking the game
            rather than assembling it here is what keeps the two in step.
        */
        std::string  line{};
    };

    /* @brief One entity the client has loaded — a player, a mob, an item. */
    struct entity_view
    {
        /* The network id, which is how packets name an entity. */
        std::int32_t id{ 0 };
        /* The class it really is, in this jar's spelling.  See class_name_of. */
        std::string  type{};
        std::string  name{};
        /* An anvil or /summon name; "" for almost everything. */
        std::string  custom_name{};
        /* What the game DRAWS above it: team colours, custom name and all. */
        std::string  display_name{};
        double       x{ 0.0 };
        double       y{ 0.0 };
        double       z{ 0.0 };
    };

    /*
        @brief The client's scoreboard for one display slot.
        @details
        Slot 0 is the tab list's column, 1 is the sidebar, 2 is the number under
        a player's nametag.  A slot with no objective is not an error: it is the
        normal state of most of them, and comes back with an empty `name`.

        THE CLIENT'S COPY, which is the only one that exists here.  It is fed by
        the server's scoreboard packets, so it holds exactly what this player has
        been told — a server that hides an objective from you has hidden it from
        this too.

        One scope for the whole read, and it is a long one: an objective, its
        scores, and a string call per row.  Bounded at 256 rows for that reason.
    */
    [[nodiscard]] inline auto scoreboard_slot(const std::int32_t slot) noexcept
        -> std::optional<objective_view>
    {
        namespace map = chatwire::mapping;
        namespace d   = chatwire::sdk::detail;
        try
        {
            const auto slot_name{ slot == 0 ? "list" : slot == 1 ? "sidebar"
                                : slot == 2 ? "belowName" : "" };
            if (slot_name[0] == '\0') { return std::nullopt; }

            const vmhook::java_thread_scope java{};
            if (!java) { return std::nullopt; }

            const auto board{ d::client_scoreboard() };
            if (!board || !board->get_instance()) { return std::nullopt; }

            objective_view out{ .slot = slot_name };

            const auto in_slot{ board->get_method(
                map::resolve(map::scoreboard.get_objective_in_display_slot),
                std::format("(I)L{};", map::resolve(map::score_objective.clazz))) };
            if (!in_slot) { return out; }

            const std::unique_ptr<d::score_objective> objective{ in_slot->call(slot) };
            if (!objective->get_instance()) { return out; }   // nothing in this slot

            out.name = d::string_call(objective, map::score_objective.get_name);
            out.display_name = d::string_call(objective, map::score_objective.get_display_name);

            const auto sorted{ board->get_method(
                map::resolve(map::scoreboard.get_sorted_scores),
                std::format("(L{};)Ljava/util/Collection;",
                            map::resolve(map::score_objective.clazz))) };
            if (!sorted) { return out; }

            const std::unique_ptr<d::java_collection> held{ sorted->call(objective) };
            if (!held->get_instance()) { return out; }

            auto rows{ d::elements_of<d::score>(held, 256u) };
            out.scores.reserve(rows.size());
            for (const auto& row : rows)
            {
                if (!row || !row->get_instance() || out.scores.size() >= 256u) { continue; }
                score_entry entry{ .name = d::string_call(row, map::score.get_player_name) };
                if (const auto points{ row->get_method(
                        map::resolve(map::score.get_score_points), "()I") })
                {
                    entry.points = points->call();
                }
                out.scores.push_back(std::move(entry));
            }
            return out;
        }
        catch (...) { return std::nullopt; }
    }

    namespace detail
    {
        /*
            @brief One ScorePlayerTeam, read into a plain struct.
            @details
            Shared by `teams()` and `team_of()` rather than written twice: the
            two commands differ in how they FIND a team, not in what a team is,
            and a second copy would be the place one of them quietly stopped
            reporting the suffix.

            Must be called inside a java_thread_scope -- it makes six calls and
            walks a collection, all against `team`'s address.
        */
        [[nodiscard]] inline auto read_team(
            const std::unique_ptr<score_player_team>& team) -> chatwire::sdk::team_view
        {
            namespace map = chatwire::mapping;
            chatwire::sdk::team_view view{
                .name = string_call(team, map::score_player_team.get_registered_name),
                .display_name = string_call(team, map::score_player_team.get_team_name),
                .prefix = string_call(team, map::score_player_team.get_color_prefix),
                .suffix = string_call(team, map::score_player_team.get_color_suffix) };

            if (const auto members{ team->get_method(
                    map::resolve(map::score_player_team.get_membership_collection),
                    "()Ljava/util/Collection;") })
            {
                const std::unique_ptr<java_collection> names{ members->call() };
                if (names->get_instance())
                {
                    // Members are java.lang.String, which vmhook decodes
                    // directly -- there is no wrapper to write for a String.
                    for (auto& who : elements_of<java_string>(names, 256u))
                    {
                        if (who && who->get_instance() && view.members.size() < 256u)
                        {
                            view.members.push_back(vmhook::read_java_string(who->get_instance()));
                        }
                    }
                }
            }
            return view;
        }
    }

    /* @brief Every team the client knows about, with its nametag decoration. */
    [[nodiscard]] inline auto teams() noexcept -> std::vector<team_view>
    {
        namespace map = chatwire::mapping;
        namespace d   = chatwire::sdk::detail;
        std::vector<team_view> out;
        try
        {
            const vmhook::java_thread_scope java{};
            if (!java) { return out; }

            const auto board{ d::client_scoreboard() };
            if (!board || !board->get_instance()) { return out; }

            const auto all{ board->get_method(map::resolve(map::scoreboard.get_teams),
                                              "()Ljava/util/Collection;") };
            if (!all) { return out; }
            const std::unique_ptr<d::java_collection> held{ all->call() };
            if (!held->get_instance()) { return out; }

            auto found{ d::elements_of<d::score_player_team>(held, 128u) };
            out.reserve(found.size());
            for (const auto& team : found)
            {
                if (!team || !team->get_instance() || out.size() >= 128u) { continue; }
                out.push_back(detail::read_team(team));
            }
        }
        catch (...) { }
        return out;
    }

    /*
        @brief The team one player is on, or nullopt when they are on none.
        @details
        BY NAME, because that is what Minecraft keys teams on -- a scoreboard
        team holds strings, not players, which is why a team can list somebody
        who is not online and why this works for them too.

        The descriptor is not optional: under OBF this method is `h`, and so is
        `getDisplaySlotStrings`, which returns a String[].  A name-only lookup
        would call that one and report nothing.
    */
    [[nodiscard]] inline auto team_of(const std::string& player) noexcept
        -> std::optional<team_view>
    {
        namespace map = chatwire::mapping;
        namespace d   = chatwire::sdk::detail;
        try
        {
            if (player.empty()) { return std::nullopt; }

            const vmhook::java_thread_scope java{};
            if (!java) { return std::nullopt; }

            const auto board{ d::client_scoreboard() };
            if (!board->get_instance()) { return std::nullopt; }

            const auto lookup{ board->get_method(
                map::resolve(map::scoreboard.get_players_team),
                std::format("(Ljava/lang/String;)L{};",
                            map::resolve(map::score_player_team.clazz))) };
            if (!lookup) { return std::nullopt; }

            const std::unique_ptr<d::score_player_team> team{ lookup->call(player) };
            if (!team->get_instance()) { return std::nullopt; }   // on no team
            return d::read_team(team);
        }
        catch (...) { return std::nullopt; }
    }

    /*
        @brief The tab list: everyone the server says is connected.
        @details
        Reached through `Minecraft.getNetHandler()`, which is null before a world
        is joined -- so this is empty on the title screen rather than failing.
    */
    [[nodiscard]] inline auto tab_list() noexcept -> std::vector<tab_entry>
    {
        namespace map = chatwire::mapping;
        namespace d   = chatwire::sdk::detail;
        std::vector<tab_entry> out;
        try
        {
            const vmhook::java_thread_scope java{};
            if (!java) { return out; }

            const auto mc{ d::client() };
            if (!mc || !mc->get_instance()) { return out; }

            const auto handler_of{ mc->get_method(
                map::resolve(map::minecraft.get_net_handler),
                d::returns(map::net_handler_play_client.clazz)) };
            if (!handler_of) { return out; }
            const std::unique_ptr<d::net_handler> handler{ handler_of->call() };
            if (!handler->get_instance()) { return out; }     // not connected

            const auto info_map{ handler->get_method(
                map::resolve(map::net_handler_play_client.get_player_info_map),
                "()Ljava/util/Collection;") };
            if (!info_map) { return out; }
            const std::unique_ptr<d::java_collection> held{ info_map->call() };
            if (!held->get_instance()) { return out; }

            auto rows{ d::elements_of<d::player_info>(held, 512u) };
            out.reserve(rows.size());
            for (const auto& row : rows)
            {
                if (!row || !row->get_instance() || out.size() >= 512u) { continue; }
                tab_entry entry{};

                if (const auto ping{ row->get_method(
                        map::resolve(map::network_player_info.get_response_time), "()I") })
                {
                    entry.ping = ping->call();
                }

                if (const auto profile_of{ row->get_method(
                        map::resolve(map::network_player_info.get_game_profile),
                        d::returns(map::game_profile.clazz)) })
                {
                    const std::unique_ptr<d::game_profile> profile{ profile_of->call() };
                    if (profile->get_instance())
                    {
                        entry.name = d::string_call(profile, map::game_profile.get_name);
                        if (const auto id{ profile->get_method(
                                map::resolve(map::game_profile.get_id), "()Ljava/util/UUID;") })
                        {
                            const std::unique_ptr<d::java_uuid> uuid{ id->call() };
                            if (uuid->get_instance())
                            {
                                if (const auto as_text{ uuid->get_method("toString") })
                                {
                                    entry.uuid = as_text->call();
                                }
                            }
                        }
                    }
                }

                entry.display_name =
                    d::component_call(row, map::network_player_info.get_display_name);
                entry.line = d::tab_line_for(row);
                // The game falls back to the profile name when the server set no
                // display name, and so does this: a caller that has to know the
                // difference can compare the two fields, and one that does not
                // gets the name the tab list draws either way.
                if (entry.display_name.empty()) { entry.display_name = entry.name; }

                if (!entry.name.empty() || !entry.uuid.empty()) { out.push_back(std::move(entry)); }
            }
        }
        catch (...) { }
        return out;
    }

    /*
        @brief Every entity the client has loaded.
        @details
        `World.loadedEntityList` — players, mobs, items, arrows, everything with
        a position.  On a busy world this is thousands of objects and a string
        call or two per entity, so it is bounded and is a request rather than
        something to poll.

        `type` is the class each one REALLY is, so a caller can filter without
        chatwire having to know what a zombie is.
    */
    [[nodiscard]] inline auto entities(const std::size_t limit = 1024u) noexcept
        -> std::vector<entity_view>
    {
        namespace map = chatwire::mapping;
        namespace d   = chatwire::sdk::detail;
        std::vector<entity_view> out;
        try
        {
            const vmhook::java_thread_scope java{};
            if (!java) { return out; }

            const auto world{ d::client_world() };
            if (!world || !world->get_instance()) { return out; }

            const auto list_proxy{ world->get_field(
                map::resolve(map::world.loaded_entity_list)) };
            if (!list_proxy) { return out; }
            const std::unique_ptr<d::java_collection> held{ list_proxy->get() };
            if (!held->get_instance()) { return out; }

            auto found{ d::elements_of<d::any_entity>(held, limit) };
            out.reserve(found.size() < limit ? found.size() : limit);
            for (const auto& entity : found)
            {
                if (!entity || !entity->get_instance() || out.size() >= limit) { continue; }
                entity_view view{ .type = d::class_name_of(entity->get_instance()) };

                if (const auto id{ entity->get_method(
                        map::resolve(map::entity.get_entity_id), "()I") })
                {
                    view.id = id->call();
                }
                view.name = d::string_call(entity, map::entity.get_name);
                view.custom_name = d::string_call(entity, map::entity.get_custom_name_tag);
                view.display_name = d::component_call(entity, map::entity.get_display_name);

                const auto load{ [&](const map::name& n, double& into) noexcept
                {
                    if (const auto f{ entity->get_field(map::resolve(n)) }) { into = f->get(); }
                } };
                load(map::entity.pos_x, view.x);
                load(map::entity.pos_y, view.y);
                load(map::entity.pos_z, view.z);

                out.push_back(std::move(view));
            }
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
        @brief Every player the client can see, with what a template can use.
        @details
        Joins the entity list (health, position) to the tab list (ping) by name,
        because neither has all of it: playerEntities knows what is loaded
        nearby, the tab list knows who the server says is connected.  A player in
        one and not the other still gets an entry, with the other half zero.
    */
    [[nodiscard]] inline auto player_snapshots() noexcept -> std::vector<player_snapshot>
    {
        std::vector<player_snapshot> out;
        try
        {
            for (const auto& entity : chatwire::sdk::entities(256u))
            {
                if (entity.name.empty()) { continue; }
                out.push_back(player_snapshot{ .name = entity.name, .x = entity.x,
                                               .y = entity.y, .z = entity.z });
            }
            for (const auto& row : chatwire::sdk::tab_list())
            {
                auto found{ std::ranges::find(out, row.name, &player_snapshot::name) };
                if (found != out.end()) { found->ping = row.ping; }
                else { out.push_back(player_snapshot{ .name = row.name, .ping = row.ping }); }
            }
            if (const auto self{ chatwire::sdk::local_player_state() })
            {
                auto found{ std::ranges::find(out, self->name, &player_snapshot::name) };
                if (found != out.end())
                {
                    found->health = self->health;
                    found->food   = self->food;
                }
            }
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
