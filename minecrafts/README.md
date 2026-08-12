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

Roughly two minutes on a warm machine, mostly downloads. If a Minecraft launcher is
installed, `%APPDATA%\.minecraft` is used as a download cache — every candidate file is
sha1-checked before it is copied, so a foreign or corrupt file falls through to the network
instead of poisoning the tree. Nothing there is written to.

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
| `status` | what exists on disk right now |
| `all` | setup + build + status |

## What it needs

- **Java 8** to run the game — 1.8.9 ships LWJGL 2.9.4 and will not start on anything newer.
  Found automatically under `C:\Program Files\Eclipse Adoptium` and friends; override with
  `MC_JAVA8`.
- **Any modern JDK** to run SpecialSource and Vineflower. Found the same way.
- **Python 3.11+**, standard library only.

## Where the pieces come from

| | |
|---|---|
| client jar, libraries, assets | `launchermeta.mojang.com` version manifest v2 |
| notch → srg (`joined.srg`) | `de.oceanlabs.mcp:mcp:1.8.9:srg@zip` |
| srg → mcp (`fields.csv`, `methods.csv`) | `de.oceanlabs.mcp:mcp_stable:22-1.8.9@zip` |
| remapper | `net.md-5:SpecialSource:1.11.0` |
| decompiler | Vineflower 1.12.0 |

`stable_22` is the last stable MCP channel published for 1.8.9, and the one every 1.8.9 mod
was written against.
