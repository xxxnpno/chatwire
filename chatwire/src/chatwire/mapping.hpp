#pragma once

// chatwire.mapping — Minecraft 1.8.9 name resolution across all three mappings.
//
// ===========================================================================
// THE PROBLEM THIS SOLVES
// ===========================================================================
// The same field in Minecraft 1.8.9 has three different names depending on how
// the jar you are attached to was built:
//
//   MCP  net/minecraft/client/Minecraft . thePlayer        (deobfuscated)
//   SRG  net/minecraft/client/Minecraft . field_71439_g    (Searge intermediate)
//   OBF  ave                            . h                (vanilla, shipped)
//
// A vanilla launcher runs OBF.  An INSTALLED Forge client runs SRG -- Forge
// reobfuscates to Searge names for release, so the client a player actually
// launches has field_71439_g, not thePlayer.  MCP is the DEVELOPMENT case: a
// ForgeGradle `runClient` or an MCP workspace, where the mod author is running
// from decompiled sources.  Getting that backwards is easy and expensive: the
// two are indistinguishable at the class level, which is why detection probes a
// FIELD.  All three are live in the wild for 1.8.9, so an API that only handles
// one of them works for a third of its users and fails confusingly for the rest.
//
// So every name in this file is a TRIPLE, and `resolve()` picks the member that
// matches whatever the attached JVM turned out to be.
//
// ===========================================================================
// HOW THE MODE IS DETECTED
// ===========================================================================
// By asking the JVM, never by guessing from a launcher name or a jar hash:
//
//   1. Does class `net/minecraft/client/Minecraft` exist?
//        yes -> does it have a field `theMinecraft`?   -> MCP
//               does it have a field `field_71432_P`?  -> SRG
//               neither                                -> unsupported build
//        no  -> does class `ave` exist?                -> OBF
//               no                                     -> not Minecraft 1.8.9
//
// The order matters.  MCP and SRG share the same CLASS names and differ only in
// MEMBER names, so the class check cannot separate them — only a field probe
// can.  OBF differs at both levels, so its class check is decisive.
//
// ===========================================================================
// WHERE THE OBF NAMES COME FROM, AND WHY THAT MATTERS
// ===========================================================================
// From the shipped jar, read with javap:
//
//     unzip -o .minecraft/versions/1.8.9/1.8.9.jar '*.class' -d out
//     javap -p -c out/ave.class
//
// NOT from a mappings site, a paste, or memory.  A wrong OBF name fails
// SILENTLY — the class resolves to something real, the member lookup finds
// nothing, and the feature that needed it just never works — so a guess here is
// indistinguishable from a correct entry until a user on a vanilla client
// reports that half the bridge does nothing.  That is exactly what `avq` for
// GuiNewChat did (see the note there).
//
// A name is only entered here once the bytecode identifies it: a field by its
// declared type, a method by the call chain it sits in.  Both are quoted at the
// entry that needed them, so the next reader can re-derive rather than trust.
#include "chatwire/common.hpp"
namespace chatwire::mapping
{
    /*
        @brief Which name set the attached Minecraft build uses.
    */
    enum class mode : std::uint8_t
    {
        /* Deobfuscated names: net/minecraft/client/Minecraft.thePlayer */
        mcp,
        /* Searge intermediate: net/minecraft/client/Minecraft.field_71439_g */
        srg,
        /* Vanilla obfuscated: ave.h */
        obf,
        /* Nothing recognisable as Minecraft 1.8.9 was found. */
        unknown,
    };

    /*
        @brief One name in all three mappings.
        @details
        A class needs only mcp/obf, because SRG leaves class names at their MCP
        spelling — that is the whole point of the intermediate mapping, which
        renames members so mods survive obfuscation churn while keeping classes
        readable.  Members need all three.
    */
    struct name
    {
        std::string_view mcp{};
        std::string_view obf{};
        std::string_view srg{};

        /*
            @brief The spelling this name has under `m`.
            @details
            An SRG lookup with no srg spelling falls back to mcp: that is the
            correct answer for JDK and library classes (java/lang/String,
            com/mojang/authlib/GameProfile), which are never remapped by anyone
            and therefore carry only one name.
        */
        [[nodiscard]] constexpr auto in(const mode m) const noexcept
            -> std::string_view
        {
            switch (m)
            {
            case mode::mcp:     return this->mcp;
            case mode::obf:     return this->obf.empty() ? this->mcp : this->obf;
            case mode::srg:     return this->srg.empty() ? this->mcp : this->srg;
            case mode::unknown: break;
            }
            return {};
        }
    };

    /*
        @brief A class plus the members chatwire touches on it.
        @details
        Only what the chat bridge needs.  This is deliberately not a general
        Minecraft SDK: every entry here is one this project actually calls, so
        an entry going stale is caught by a feature failing rather than by
        silent bit-rot in a table nobody reads.
    */

    // net.minecraft.client.Minecraft — the client singleton.
    namespace minecraft
    {
        inline constexpr name clazz{ .mcp = "net/minecraft/client/Minecraft", .obf = "ave" };
        inline constexpr name the_minecraft{ .mcp = "theMinecraft", .obf = "S", .srg = "field_71432_P" };
        inline constexpr name the_player{ .mcp = "thePlayer", .obf = "h", .srg = "field_71439_g" };
        inline constexpr name ingame_gui{ .mcp = "ingameGUI", .obf = "q", .srg = "field_71456_v" };
        /* The world the player is in, or null on the title screen. */
        inline constexpr name the_world{ .mcp = "theWorld", .obf = "f", .srg = "field_71441_e" };
        /*
            loadWorld(WorldClient[, String]) — the client changing world.  Every
            join, respawn, server switch and disconnect goes through it, and a
            NULL world argument is the disconnect: it is the one method that says
            "the world you knew is gone" before theWorld stops being readable.

            Both overloads share this name, and the one-argument form is a
            one-line delegation to the two-argument one:

                a(Lbdb;)V         aload_0 aload_1 ldc "" invokevirtual a(Lbdb;Ljava/lang/String;)V

            so hooking the LONGER descriptor catches every call.  The descriptor
            is built at run time rather than spelled here -- see
            sdk::install_world_observer -- because it contains WorldClient's
            class name, and the honest source for that is the attached jar.
        */
        inline constexpr name load_world{ .mcp = "loadWorld", .obf = "a", .srg = "func_71353_a" };
        /*
            The ONE-argument overload, and it needs an entry of its own for one
            reason: SRG gives the two overloads DIFFERENT names.

                func_71353_a   loadWorld(WorldClient, String)
                func_71403_a   loadWorld(WorldClient)

            MCP calls both `loadWorld` and OBF calls both `a`, so under those two
            a name plus a descriptor is enough and this entry is the same string
            twice.  Under SRG it is not, and looking for the long name with the
            short descriptor finds nothing -- which is exactly how a fallback
            path rots unnoticed, since the primary works and nobody exercises it.

            Only a fallback.  The short form delegates to the long one, so a
            client where both exist is caught entirely by load_world above.
        */
        inline constexpr name load_world_short{ .mcp = "loadWorld", .obf = "a", .srg = "func_71403_a" };
        // runTick is deliberately absent.  It was the pump's hook target, and
        // there is no pump: vmhook can enter Java on any thread now, so nothing
        // has to be marshalled onto the client's main loop.  This table is only
        // what chatwire touches, so an entry nothing calls does not sit here
        // going stale.
    }

    // net.minecraft.client.entity.EntityPlayerSP — the local player.
    namespace entity_player_sp
    {
        inline constexpr name clazz{ .mcp = "net/minecraft/client/entity/EntityPlayerSP", .obf = "bew" };
        /* sendChatMessage(String) — goes to the SERVER, as if typed. */
        inline constexpr name send_chat_message{ .mcp = "sendChatMessage", .obf = "e", .srg = "func_71165_d" };
        /* addChatMessage(IChatComponent) — CLIENT-side only, never transmitted. */
        inline constexpr name add_chat_message{ .mcp = "addChatMessage", .obf = "a", .srg = "func_145747_a" };
    }

    // net.minecraft.client.multiplayer.WorldClient — the client's world.  Only
    // the DECLARED type of Minecraft.theWorld matters here: a JNI field lookup
    // is by declared type, not by what the object turns out to be.
    //
    //     ave.f is bdb, and bdb extends adm (World).
    namespace world_client
    {
        inline constexpr name clazz{ .mcp = "net/minecraft/client/multiplayer/WorldClient",
                                     .obf = "bdb" };
    }

    // net.minecraft.world.World — where the player list lives.  `playerEntities`
    // is declared on World and inherited by WorldClient, which is why a lookup
    // against the instance finds it.
    //
    //     adm holds seven List fields; j is the only List<wn>, and wn is
    //     EntityPlayer (bew -> bet -> wn).  That is what identifies it.
    namespace world
    {
        inline constexpr name clazz{ .mcp = "net/minecraft/world/World", .obf = "adm" };
        /* List<EntityPlayer> — everyone the client currently knows about. */
        inline constexpr name player_entities{ .mcp = "playerEntities", .obf = "j",
                                               .srg = "field_73010_i" };
    }

    // net.minecraft.entity.Entity — the two identity accessors every player has.
    // Declared on Entity and overridden on EntityPlayer, so a virtual call
    // against a player instance lands on the player's version.
    //
    //     pk (Entity) declares exactly one no-argument UUID getter, aK, and
    //     e_ is the String getter EntityPlayer overrides.
    namespace entity
    {
        /* getName() — the display name, which is the nickname on most servers. */
        inline constexpr name get_name{ .mcp = "getName", .obf = "e_", .srg = "func_70005_c_" };
        /* getUniqueID() — the account UUID, stable across name changes. */
        inline constexpr name get_unique_id{ .mcp = "getUniqueID", .obf = "aK",
                                             .srg = "func_110124_au" };
    }

    // net.minecraft.client.gui.GuiIngame — holds the chat GUI.
    namespace gui_ingame
    {
        inline constexpr name clazz{ .mcp = "net/minecraft/client/gui/GuiIngame", .obf = "avo" };
        inline constexpr name persistant_chat_gui{ .mcp = "persistantChatGUI", .obf = "l", .srg = "field_73840_e" };
    }

    // net.minecraft.client.gui.GuiNewChat — every line that reaches the chat box
    // passes through printChatMessage, which is why hooking it once catches
    // everything: server messages, client messages, mod output, death messages.
    //
    // THE OBF NAME WAS WRONG HERE, and silently: it said `avq`, which in 1.8.9
    // is MapItemRenderer -- a class that exists, registers cleanly, and has no
    // method called `a(Leu;)V`, so the chat observer simply never installed on a
    // vanilla client and every OBF user got a bridge that could send chat and
    // never reported any.  The right answer is `avt`, and the shipped bytecode
    // says so end to end: EntityPlayerSP.addChatMessage is
    //
    //     bew.a(Leu;)V   getfield ave.q:Lavo;   invokevirtual avo.d:()Lavt;
    //                    invokevirtual avt.a:(Leu;)V
    //
    // which is `this.mc.ingameGUI.getChatGUI().printChatMessage(component)` with
    // every link named.  avt also has the shape: it extends avp (Gui), holds a
    // List<String> of sent messages and two List<ava> of chat lines, and carries
    // a(Leu;), a(Leu;I) and a private a(Leu;III) -- printChatMessage,
    // printChatMessageWithOptionalDeletion and setChatLine.
    namespace gui_new_chat
    {
        inline constexpr name clazz{ .mcp = "net/minecraft/client/gui/GuiNewChat", .obf = "avt" };
        inline constexpr name print_chat_message{ .mcp = "printChatMessage", .obf = "a", .srg = "func_146227_a" };
    }

    // net.minecraft.util.IChatComponent — the rich-text interface every chat
    // line is built from.
    namespace i_chat_component
    {
        inline constexpr name clazz{ .mcp = "net/minecraft/util/IChatComponent", .obf = "eu" };
        /* With the section-sign colour codes still in. */
        inline constexpr name get_formatted_text{ .mcp = "getFormattedText", .obf = "d", .srg = "func_150254_d" };
        /* Plain text, colour codes stripped. */
        inline constexpr name get_unformatted_text{ .mcp = "getUnformattedText", .obf = "c", .srg = "func_150260_c" };
    }

    // net.minecraft.util.ChatComponentText — the simplest IChatComponent, built
    // from a plain String.  What add_chat_message wraps its text in.
    namespace chat_component_text
    {
        inline constexpr name clazz{ .mcp = "net/minecraft/util/ChatComponentText", .obf = "fa" };
    }

    /* @brief The detected mode.  mode::unknown until detect() succeeds. */
    inline mode current{ mode::unknown };

    /* @brief The spelling of `n` under the detected mode. */
    [[nodiscard]] inline auto resolve(const name& n) noexcept
        -> std::string
    {
        return std::string{ n.in(current) };
    }

    /* @brief Human-readable mode, for logs. */
    [[nodiscard]] constexpr auto mode_name(const mode m) noexcept
        -> std::string_view
    {
        switch (m)
        {
        case mode::mcp:     return "MCP (deobfuscated)";
        case mode::srg:     return "SRG (Searge intermediate)";
        case mode::obf:     return "OBF (vanilla obfuscated)";
        case mode::unknown: break;
        }
        return "unknown";
    }

    /*
        @brief What a caller must probe in the JVM for detect() to decide.
        @details
        mapping owns the DECISION but not the probing: probing needs vmhook, and
        keeping vmhook out of this module is what lets every other module import
        the name tables freely.  chatwire.sdk does the three lookups and hands
        the answers here.
    */
    struct probe_result
    {
        /* Does class `net/minecraft/client/Minecraft` exist? */
        bool mcp_class_present{ false };
        /* Does it have a field spelled `theMinecraft`? */
        bool mcp_field_present{ false };
        /* Does it have a field spelled `field_71432_P`? */
        bool srg_field_present{ false };
        /* Does class `ave` exist? */
        bool obf_class_present{ false };
    };

    /*
        @brief Decides the mapping from probe results, and sets `current`.
        @details
        The order matters.  MCP and SRG share CLASS names and differ only in
        MEMBER names, so the class check cannot separate them — only a field
        probe can.  OBF differs at both levels, so its class check is decisive.
    */
    inline auto decide(const probe_result& probe) noexcept
        -> mode
    {
        if (probe.mcp_class_present)
        {
            if (probe.mcp_field_present)      { current = mode::mcp; }
            else if (probe.srg_field_present) { current = mode::srg; }
            else                              { current = mode::unknown; }
            return current;
        }
        if (probe.obf_class_present) { current = mode::obf; return current; }
        current = mode::unknown;
        return current;
    }
}
