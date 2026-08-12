# minecrafts

Three Minecraft 1.8.9 clients that are the same game under three different sets of names.

```
                 Mojang's client.jar
                          |
   vanilla  ──────────────┘                a.class,  a.a(), a.J
     |  joined.srg                         mcp-1.8.9-srg.zip
   srg      net/minecraft/client/Minecraft.class, func_71407_l(), field_71425_J
     |  srg-to-mcp.srg  (from fields.csv + methods.csv)
   mcp      net/minecraft/client/Minecraft.class, runTick(),      running
```

chatwire attaches to a *running* JVM, so it only ever sees the names that jar actually
contains. Having all three side by side is how a name written in any mapping gets checked
against the one the process really has — see `shared/mappings/1.8.9-mappings.json`, which
carries all three names for every class, field and method.

Nothing in here is committed. `minecrafts/mc.py` and `minecrafts/mcbuild/` are; everything
they produce is ignored, because it is ~350 MB of other people's bytes.

## Build it

```bash
python minecrafts/mc.py all          # setup + build + status, from nothing
```

Roughly three minutes on a warm machine, mostly downloads. If a Minecraft launcher is
installed, `%APPDATA%\.minecraft` is used as a download cache — every candidate file is
sha1-checked before it is copied, so a foreign or corrupt file falls through to the network
instead of poisoning the tree. Nothing there is written to.

That includes **Mojang's own JVM**: the version manifest names `jre-legacy` (Java 8u51) and
setup installs exactly that into `shared/runtime`. Not a detail — see *The JVM is not
interchangeable* below.

## Run it

```bash
python minecrafts/mc.py launch vanilla        # or srg, or mcp
python minecrafts/mc.py launch mcp --timeout 60
python minecrafts/mc.py launch --all          # all three at once, detached
```

Each mapping gets its own `run/` game directory, so worlds, `options.txt` and logs never
collide; they share one assets tree and one natives directory. `launch --all` gives each
client a distinct username (`chatwire_van`, `chatwire_srg`, `chatwire_mcp`) so a server can
tell them apart.

`launch` also passes `-Dminecraft.mapping=<name>` to the JVM — a hooked process can read that
system property to know which mapping it landed in without inspecting a single class.

Sessions are offline (`--accessToken 0`). Singleplayer and LAN work; joining an online-mode
server does not.

`python mc.py build` also writes `launch-vanilla.bat`, `launch-srg.bat` and `launch-mcp.bat`
next to `mc.py`, for starting a client without Python in the way.

## Read it

```bash
python minecrafts/mc.py decompile          # all three, several minutes each
python minecrafts/mc.py decompile mcp      # just the readable one
```

Vineflower writes `<mapping>/src`. The vanilla tree is the interesting one: `a.class` and
`A.class` are two classes to the JVM and one filename to Windows, so colliding names get a
`#1` suffix rather than silently overwriting each other.

## Subcommands

| | |
|---|---|
| `setup` | download Mojang's files and the MCP mappings |
| `build [mapping...]` | produce the three jars, and the `.bat` launchers |
| `decompile [mapping...]` | Vineflower each jar into `<mapping>/src` |
| `launch <mapping>` | start one; `--all`, `--detach`, `--timeout N`, `--username`, `--memory` |
| `check` | check chatwire's name table against the jar, offline |
| `chatwire` | server + all three clients + a chatwire in each, then ask them all |
| `server` | run the local 1.8.9 server on its own |
| `status` | what exists on disk right now |
| `all` | setup + build + status |

## Checking chatwire against them

Two checks, and the cheap one runs without starting anything:

```bash
python minecrafts/mc.py check       # offline: mapping.hpp vs the real mappings
python minecrafts/mc.py chatwire    # live: all three clients, three chatwires at once
```

`check` parses every `name` triple out of `chatwire/src/chatwire/mapping.hpp` and looks each one
up in `1.8.9-mappings.json`, resolving inherited members through the class hierarchy read
straight out of the jar's class files. It is the check that would have caught `avq`.

`chatwire` is the live one, and it is a whole world rather than three title screens. It starts
Mojang's own 1.8.9 server — offline, flat, creative, with the three test accounts opped — brings
up all three clients with `--server 127.0.0.1` so they join on launch, injects a separate
chatwire into each **at the same time** on ports 24455-24457, and then asks every one of them
everything. Three bridges into three JVMs is what a user driving several accounts has, and
running them one at a time would never exercise it.

Almost nothing chatwire reads exists at the main menu, which is why the server is not optional
for a real check: `thePlayer` is null, there is no world, and a third of the name table is not
even loaded. `--no-server` skips it and the in-world half.

The strongest check in there is the chat round trip. `sendChatMessage` starts as JSON on a
socket, becomes a Java call inside the game, becomes a packet, and comes back out in another
process's log — and `addChatMessage` never leaves the client but comes back through chatwire's
own hook on `printChatMessage`. Neither can pass by accident.

```
* vanilla on port 24455
    mapping.detected   'OBF (vanilla obfuscated)' (minecraft_class=False ... obf_class=True)
    mapping.verify     checked=42 missing=0 unchecked=0
    mapping.resolve    'printChatMessage' -> 'a' (field and method, on gui_new_chat)
    thePlayer          'chatwire_van' at (213.50, 4.00, -520.50) hp=20 food=20 dim=0
    currentScreen      open=False ''
    playerEntities     3 loaded: chatwire_mcp, chatwire_srg, chatwire_van
    sendChatMessage    sent=True
    server heard it    yes
    addChatMessage     shown and observed back: True
* srg on port 24456
    mapping.resolve    'printChatMessage' -> 'func_146227_a' (method, on gui_new_chat)
    currentScreen      open=True 'net/minecraft/client/gui/GuiIngameMenu'
* mcp on port 24457
    mapping.resolve    'printChatMessage' -> 'printChatMessage' (method, on gui_new_chat)
```

`missing` is a name chatwire has wrong. `unchecked` is a class Minecraft has not loaded yet —
`FoodStats` does not exist until a player does — and the two are reported separately because
only the first is a bug.

`--keep` leaves the clients up so you can drive them yourself.

## The JVM is not interchangeable

A JVM is not just what runs the game. It is what chatwire *reads*: vmhook resolves HotSpot's
internals through the `gHotSpotVMStructs` table each build exports, and builds differ.

This tree originally launched with whatever Java 8 was installed, which here was Adoptium
8u492 — eleven years newer than anything a 1.8.9 player runs. On that build chatwire could not
resolve a single class, `java/lang/String` included, and reported "no supported Minecraft 1.8.9
found". On Mojang's `jre-legacy` it works. Testing against a JVM nobody runs proves nothing
about the ones they do, so the runtime is pinned by the version manifest like everything else.

The same investigation turned up a real defect in vmhook's JDK 8 path, fixed in this branch:
`BasicHashtableEntry` keeps `_hash` at +0 and `_next` at +8, and the dictionary walk read
`_next` from +0. Every bucket chain ended at its first entry, so a client with 5000 loaded
classes looked like it had 1015 — almost exactly the JDK 8 SystemDictionary's 1009 buckets.
It never crashed and never logged; it just could not find most classes.

## What it needs

- **Java 8** to run the game — 1.8.9 ships LWJGL 2.9.4 and will not start on anything newer.
  `mc.py setup` downloads Mojang's, which is the one to use. A system Java 8 is the fallback,
  found under `C:\Program Files\Eclipse Adoptium` and friends; override with `MC_JAVA8`.
- **Any modern JDK** to run SpecialSource and Vineflower. Found the same way.
- **Python 3.11+**, standard library only.

## Where the pieces come from

| | |
|---|---|
| client jar, libraries, assets | `launchermeta.mojang.com` version manifest v2 |
| the JVM (`jre-legacy`, 8u51) | Mojang's java-runtime manifest |
| notch → srg (`joined.srg`) | `de.oceanlabs.mcp:mcp:1.8.9:srg@zip` |
| srg → mcp (`fields.csv`, `methods.csv`) | `de.oceanlabs.mcp:mcp_stable:22-1.8.9@zip` |
| remapper | `net.md-5:SpecialSource:1.11.0` |
| decompiler | Vineflower 1.12.0 |

`stable_22` is the last stable MCP channel published for 1.8.9, and the one every 1.8.9 mod
was written against.
