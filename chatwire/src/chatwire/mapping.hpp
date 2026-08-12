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

        ===================================================================
        WHY EACH GROUP IS A STRUCT AND NOT A NAMESPACE
        ===================================================================
        A namespace's members cannot be ENUMERATED.  These were namespaces, and
        that is precisely why the `avq` mistake could sit here for a release:
        nothing could walk the table and ask the JVM whether each entry was
        real, because nothing could ask what the entries were.

        A struct can be walked.  `std::meta::nonstatic_data_members_of` gives
        every entry with its identifier, which is what lets sdk::verify_mapping
        check the whole table against the attached client and lets the
        `mapping.verify` command report the answer.  Adding a name below is
        therefore adding a name that gets verified -- there is no second list to
        remember to update, which is the same property json.hpp gets from the
        same mechanism.

        THE CONVENTION EVERY GROUP FOLLOWS, and which the walk relies on:

            the member called `clazz` is the CLASS;
            every other member is a field or a method ON that class.

        So a group with members needs a `clazz`, even when chatwire never names
        that class anywhere else -- see `entity_names`, whose two accessors are
        declared on Entity and reached through a player.

        The short names below (`mapping::minecraft`, `mapping::world`) are
        references INTO the single `table` instance rather than objects of their
        own, so there is exactly one list and a group cannot be added to the
        table and forgotten at its call site, or the reverse.
    */

    // net.minecraft.client.Minecraft — the client singleton.
    struct minecraft_names
    {
        name clazz{ .mcp = "net/minecraft/client/Minecraft", .obf = "ave" };
        name the_minecraft{ .mcp = "theMinecraft", .obf = "S", .srg = "field_71432_P" };
        name the_player{ .mcp = "thePlayer", .obf = "h", .srg = "field_71439_g" };
        name ingame_gui{ .mcp = "ingameGUI", .obf = "q", .srg = "field_71456_v" };
        /* The world the player is in, or null on the title screen. */
        name the_world{ .mcp = "theWorld", .obf = "f", .srg = "field_71441_e" };
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
        name load_world{ .mcp = "loadWorld", .obf = "a", .srg = "func_71353_a" };
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
        name load_world_short{ .mcp = "loadWorld", .obf = "a", .srg = "func_71403_a" };
        /*
            The GuiScreen currently open, or null when the player is looking at
            the world.  chatwire only asks whether it is null and what class it
            is, which together answer "is the player in a menu, and which one" --
            the question a client needs before pretending to type.
        */
        name current_screen{ .mcp = "currentScreen", .obf = "m", .srg = "field_71462_r" };
        /* getNetHandler() - the connection, and through it the tab list. */
        name get_net_handler{ .mcp = "getNetHandler", .obf = "u", .srg = "func_147114_u" };
        // runTick is deliberately absent.  It was the pump's hook target, and
        // there is no pump: vmhook can enter Java on any thread now, so nothing
        // has to be marshalled onto the client's main loop.  This table is only
        // what chatwire touches, so an entry nothing calls does not sit here
        // going stale.
    };

    // net.minecraft.client.entity.EntityPlayerSP — the local player.
    struct entity_player_sp_names
    {
        name clazz{ .mcp = "net/minecraft/client/entity/EntityPlayerSP", .obf = "bew" };
        /* sendChatMessage(String) — goes to the SERVER, as if typed. */
        name send_chat_message{ .mcp = "sendChatMessage", .obf = "e", .srg = "func_71165_d" };
        /* addChatMessage(IChatComponent) — CLIENT-side only, never transmitted. */
        name add_chat_message{ .mcp = "addChatMessage", .obf = "a", .srg = "func_145747_a" };
    };

    // net.minecraft.client.multiplayer.WorldClient — the client's world.  Only
    // the DECLARED type of Minecraft.theWorld matters here: a JNI field lookup
    // is by declared type, not by what the object turns out to be.
    //
    //     ave.f is bdb, and bdb extends adm (World).
    struct world_client_names
    {
        name clazz{ .mcp = "net/minecraft/client/multiplayer/WorldClient", .obf = "bdb" };
    };

    // net.minecraft.world.World — where the player list lives.  `playerEntities`
    // is declared on World and inherited by WorldClient, which is why a lookup
    // against the instance finds it.
    //
    //     adm holds seven List fields; j is the only List<wn>, and wn is
    //     EntityPlayer (bew -> bet -> wn).  That is what identifies it.
    struct world_names
    {
        name clazz{ .mcp = "net/minecraft/world/World", .obf = "adm" };
        /* List<EntityPlayer> — everyone the client currently knows about. */
        name player_entities{ .mcp = "playerEntities", .obf = "j", .srg = "field_73010_i" };
        /* List<Entity> - EVERYTHING the client has loaded, players included. */
        name loaded_entity_list{ .mcp = "loadedEntityList", .obf = "f", .srg = "field_72996_f" };
        /* getScoreboard() - the client's copy, fed by the server's packets. */
        name get_scoreboard{ .mcp = "getScoreboard", .obf = "Z", .srg = "func_96441_U" };
    };

    // net.minecraft.scoreboard.Scoreboard - objectives, scores and teams.
    //
    // TWO OF THESE NEED THEIR DESCRIPTOR AND WILL SILENTLY MISBEHAVE WITHOUT IT.
    // Under OBF `getPlayersTeam` and `getDisplaySlotStrings` are both `h`, and
    // `getSortedScores` and `getObjectiveDisplaySlotNumber` are both `i`.  A
    // name-only lookup takes whichever comes first in the method array, which is
    // how a scoreboard reader ends up calling the one that returns a String[]
    // and reporting nothing at all.
    struct scoreboard_names
    {
        name clazz{ .mcp = "net/minecraft/scoreboard/Scoreboard", .obf = "auo" };
        /* getObjectiveInDisplaySlot(int) - 0 list, 1 sidebar, 2 below name. */
        name get_objective_in_display_slot{ .mcp = "getObjectiveInDisplaySlot", .obf = "a",
                                            .srg = "func_96539_a" };
        /* getSortedScores(ScoreObjective) - highest first, as the sidebar draws. */
        name get_sorted_scores{ .mcp = "getSortedScores", .obf = "i", .srg = "func_96534_i" };
        name get_teams{ .mcp = "getTeams", .obf = "g", .srg = "func_96525_g" };
        name get_players_team{ .mcp = "getPlayersTeam", .obf = "h", .srg = "func_96509_i" };
    };

    // net.minecraft.scoreboard.ScoreObjective - one scoreboard column.
    struct score_objective_names
    {
        name clazz{ .mcp = "net/minecraft/scoreboard/ScoreObjective", .obf = "auk" };
        /* The internal name, which is what commands refer to. */
        name get_name{ .mcp = "getName", .obf = "b", .srg = "func_96679_b" };
        /* The title drawn above the sidebar, section signs and all. */
        name get_display_name{ .mcp = "getDisplayName", .obf = "d", .srg = "func_96678_d" };
    };

    // net.minecraft.scoreboard.Score - one row of one objective.
    struct score_names
    {
        name clazz{ .mcp = "net/minecraft/scoreboard/Score", .obf = "aum" };
        name get_score_points{ .mcp = "getScorePoints", .obf = "c", .srg = "func_96652_c" };
        /*
            The "player" a score belongs to, which on most servers is not a
            player at all: sidebars are built out of fake entries whose names are
            the text you see.  Reported as-is rather than filtered.
        */
        name get_player_name{ .mcp = "getPlayerName", .obf = "e", .srg = "func_96653_e" };
    };

    // net.minecraft.scoreboard.ScorePlayerTeam - a team, and the nametag
    // decoration every member of it gets.
    struct score_player_team_names
    {
        name clazz{ .mcp = "net/minecraft/scoreboard/ScorePlayerTeam", .obf = "aul" };
        /* The team's id, which is what /team commands use. */
        name get_registered_name{ .mcp = "getRegisteredName", .obf = "b", .srg = "func_96661_b" };
        /* The display name, which is what a tab header shows. */
        name get_team_name{ .mcp = "getTeamName", .obf = "c", .srg = "func_96669_c" };
        name get_membership_collection{ .mcp = "getMembershipCollection", .obf = "d",
                                        .srg = "func_96670_d" };
        /*
            Prefix and suffix WITH the team's colour already applied - these are
            the strings the game puts either side of a member's name, which is
            what makes one nametag red and another blue.  Not the raw fields.
        */
        name get_color_prefix{ .mcp = "getColorPrefix", .obf = "e", .srg = "func_96668_e" };
        name get_color_suffix{ .mcp = "getColorSuffix", .obf = "f", .srg = "func_96663_f" };
    };

    // net.minecraft.client.network.NetHandlerPlayClient - the connection, and
    // the only place the TAB LIST exists.  It is not the world's player list:
    // playerEntities is who is loaded nearby, this is everyone on the server.
    struct net_handler_play_client_names
    {
        name clazz{ .mcp = "net/minecraft/client/network/NetHandlerPlayClient", .obf = "bcy" };
        name get_player_info_map{ .mcp = "getPlayerInfoMap", .obf = "d", .srg = "func_175106_d" };
    };

    // net.minecraft.client.network.NetworkPlayerInfo - one tab-list row.
    struct network_player_info_names
    {
        name clazz{ .mcp = "net/minecraft/client/network/NetworkPlayerInfo", .obf = "bdc" };
        name get_game_profile{ .mcp = "getGameProfile", .obf = "a", .srg = "func_178845_a" };
        /* Ping in milliseconds, which is what the tab list draws its bars from. */
        name get_response_time{ .mcp = "getResponseTime", .obf = "c", .srg = "func_178853_c" };
        /*
            The tab-list nametag, or null when the server has not set one - in
            which case the game falls back to the profile name, and so does this.
        */
        name get_display_name{ .mcp = "getDisplayName", .obf = "k", .srg = "func_178854_k" };
    };

    // com.mojang.authlib.GameProfile - a LIBRARY class, so it carries one name
    // in every mapping and `name::in` falls back to mcp for all three.  Its two
    // accessors are not remapped either.
    struct game_profile_names
    {
        name clazz{ .mcp = "com/mojang/authlib/GameProfile" };
        name get_name{ .mcp = "getName" };
        name get_id{ .mcp = "getId" };
    };

    // net.minecraft.entity.Entity — the two identity accessors every player has.
    // Declared on Entity and overridden on EntityPlayer, so a virtual call
    // against a player instance lands on the player's version.
    //
    //     pk (Entity) declares exactly one no-argument UUID getter, aK, and
    //     e_ is the String getter EntityPlayer overrides.
    //
    // The class itself is named here even though chatwire never registers it or
    // looks anything up on it directly -- the calls go through a player.  It is
    // here because the verification walk needs a class to check these two
    // against, and "the class that declares them" is the only honest answer.
    struct entity_names
    {
        name clazz{ .mcp = "net/minecraft/entity/Entity", .obf = "pk" };
        /* getName() — the display name, which is the nickname on most servers. */
        name get_name{ .mcp = "getName", .obf = "e_", .srg = "func_70005_c_" };
        /* getUniqueID() — the account UUID, stable across name changes. */
        name get_unique_id{ .mcp = "getUniqueID", .obf = "aK", .srg = "func_110124_au" };
        /* getEntityId() - the network id, which is how packets name an entity. */
        name get_entity_id{ .mcp = "getEntityId", .obf = "F", .srg = "func_145782_y" };
        /*
            The NAMETAG, in its two halves.  `getCustomNameTag` is the string an
            anvil or a /summon put on the entity and is "" for most of them;
            `getDisplayName` is what the game actually draws, team colours and
            all, and is an IChatComponent rather than a String.
        */
        name get_custom_name_tag{ .mcp = "getCustomNameTag", .obf = "aM", .srg = "func_95999_t" };
        name has_custom_name{ .mcp = "hasCustomName", .obf = "l_", .srg = "func_145818_k_" };
        name get_display_name{ .mcp = "getDisplayName", .obf = "f_", .srg = "func_145748_c_" };
        /*
            Position and facing, as plain fields.  Every one of these is declared
            on Entity and inherited all the way down to EntityPlayerSP, so they
            resolve against the local player without naming a subclass.

            posX/posY/posZ are doubles and yaw/pitch are floats: Minecraft's own
            types, kept rather than widened, because a client comparing a
            position to one it got from a packet wants the same number.
        */
        name pos_x{ .mcp = "posX", .obf = "s", .srg = "field_70165_t" };
        name pos_y{ .mcp = "posY", .obf = "t", .srg = "field_70163_u" };
        name pos_z{ .mcp = "posZ", .obf = "u", .srg = "field_70161_v" };
        name rotation_yaw{ .mcp = "rotationYaw", .obf = "y", .srg = "field_70177_z" };
        name rotation_pitch{ .mcp = "rotationPitch", .obf = "z", .srg = "field_70125_A" };
        name on_ground{ .mcp = "onGround", .obf = "C", .srg = "field_70122_E" };
        name dimension{ .mcp = "dimension", .obf = "am", .srg = "field_71093_bK" };
    };

    // net.minecraft.entity.EntityLivingBase — where health lives, and the one
    // value in the player snapshot that is not a field.  1.8.9 keeps health in
    // the DataWatcher rather than in a member, so getHealth() is the only honest
    // way to read it; everything else below is a plain load.
    struct entity_living_base_names
    {
        name clazz{ .mcp = "net/minecraft/entity/EntityLivingBase", .obf = "pr" };
        name get_health{ .mcp = "getHealth", .obf = "bn", .srg = "func_110143_aJ" };
    };

    // net.minecraft.entity.player.EntityPlayer — the player-only part of the
    // state, declared here and inherited by EntityPlayerSP.
    struct entity_player_names
    {
        name clazz{ .mcp = "net/minecraft/entity/player/EntityPlayer", .obf = "wn" };
        /* FoodStats — hunger, which is an object rather than a number. */
        name food_stats{ .mcp = "foodStats", .obf = "bl", .srg = "field_71100_bB" };
        name experience_level{ .mcp = "experienceLevel", .obf = "bB", .srg = "field_71068_ca" };
        /* InventoryPlayer — the 36 main slots plus armour and the held stack. */
        name inventory{ .mcp = "inventory", .obf = "bi", .srg = "field_71071_by" };
    };

    // net.minecraft.util.FoodStats — hunger, saturation and the exhaustion that
    // drives them.  All plain fields, so no call is needed to read the bar.
    struct food_stats_names
    {
        name clazz{ .mcp = "net/minecraft/util/FoodStats", .obf = "xg" };
        name food_level{ .mcp = "foodLevel", .obf = "a", .srg = "field_75127_a" };
        name food_saturation_level{ .mcp = "foodSaturationLevel", .obf = "b",
                                    .srg = "field_75125_b" };
    };

    // net.minecraft.client.gui.GuiScreen — only ever used as the declared type
    // of Minecraft.currentScreen.  Which screen it really is comes from the
    // object's own klass, not from this name.
    struct gui_screen_names
    {
        name clazz{ .mcp = "net/minecraft/client/gui/GuiScreen", .obf = "axu" };
    };

    // net.minecraft.client.gui.GuiIngame — holds the chat GUI.
    struct gui_ingame_names
    {
        name clazz{ .mcp = "net/minecraft/client/gui/GuiIngame", .obf = "avo" };
        name persistant_chat_gui{ .mcp = "persistantChatGUI", .obf = "l", .srg = "field_73840_e" };
    };

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
    struct gui_new_chat_names
    {
        name clazz{ .mcp = "net/minecraft/client/gui/GuiNewChat", .obf = "avt" };
        name print_chat_message{ .mcp = "printChatMessage", .obf = "a", .srg = "func_146227_a" };
    };

    // net.minecraft.util.IChatComponent — the rich-text interface every chat
    // line is built from.
    struct i_chat_component_names
    {
        name clazz{ .mcp = "net/minecraft/util/IChatComponent", .obf = "eu" };
        /* With the section-sign colour codes still in. */
        name get_formatted_text{ .mcp = "getFormattedText", .obf = "d", .srg = "func_150254_d" };
        /* Plain text, colour codes stripped. */
        name get_unformatted_text{ .mcp = "getUnformattedText", .obf = "c", .srg = "func_150260_c" };
    };

    // net.minecraft.util.ChatComponentText — the simplest IChatComponent, built
    // from a plain String.  What add_chat_message wraps its text in.
    struct chat_component_text_names
    {
        name clazz{ .mcp = "net/minecraft/util/ChatComponentText", .obf = "fa" };
    };

    /*
        @brief Every group, in one walkable object.
        @details
        THE list.  Adding a class chatwire touches means adding a member here
        and nowhere else: the reference below makes it reachable by its short
        name, and the reflective walk in sdk::verify_mapping picks it up without
        being told.

        Order is the order a reader meets them -- the client, the player, the
        world, then the chat GUI -- because it is also the order `mapping.verify`
        reports in.
    */
    struct table
    {
        minecraft_names           minecraft{};
        entity_player_sp_names    entity_player_sp{};
        world_client_names        world_client{};
        world_names               world{};
        entity_names              entity{};
        entity_living_base_names  entity_living_base{};
        scoreboard_names          scoreboard{};
        score_objective_names     score_objective{};
        score_names               score{};
        score_player_team_names   score_player_team{};
        net_handler_play_client_names net_handler_play_client{};
        network_player_info_names network_player_info{};
        game_profile_names        game_profile{};
        entity_player_names       entity_player{};
        food_stats_names          food_stats{};
        gui_ingame_names          gui_ingame{};
        gui_new_chat_names        gui_new_chat{};
        gui_screen_names          gui_screen{};
        i_chat_component_names    i_chat_component{};
        chat_component_text_names chat_component_text{};
    };

    /* @brief The one instance.  Every name in the project resolves through it. */
    inline constexpr table all{};

    // The short spellings, as REFERENCES into `all` rather than objects of their
    // own.  Two copies of the same table would be two things to keep in step,
    // and the one that call sites used would be the one that never got verified.
    inline constexpr const minecraft_names&           minecraft{ all.minecraft };
    inline constexpr const entity_player_sp_names&    entity_player_sp{ all.entity_player_sp };
    inline constexpr const world_client_names&        world_client{ all.world_client };
    inline constexpr const world_names&               world{ all.world };
    inline constexpr const entity_names&              entity{ all.entity };
    inline constexpr const entity_living_base_names&  entity_living_base{ all.entity_living_base };
    inline constexpr const scoreboard_names&          scoreboard{ all.scoreboard };
    inline constexpr const score_objective_names&     score_objective{ all.score_objective };
    inline constexpr const score_names&               score{ all.score };
    inline constexpr const score_player_team_names&   score_player_team{ all.score_player_team };
    inline constexpr const net_handler_play_client_names& net_handler_play_client{
        all.net_handler_play_client };
    inline constexpr const network_player_info_names& network_player_info{
        all.network_player_info };
    inline constexpr const game_profile_names&        game_profile{ all.game_profile };
    inline constexpr const entity_player_names&       entity_player{ all.entity_player };
    inline constexpr const food_stats_names&          food_stats{ all.food_stats };
    inline constexpr const gui_ingame_names&          gui_ingame{ all.gui_ingame };
    inline constexpr const gui_new_chat_names&        gui_new_chat{ all.gui_new_chat };
    inline constexpr const gui_screen_names&          gui_screen{ all.gui_screen };
    inline constexpr const i_chat_component_names&    i_chat_component{ all.i_chat_component };
    inline constexpr const chat_component_text_names& chat_component_text{ all.chat_component_text };

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
