# chatwire

A live WebSocket API into a running **Minecraft 1.8.9** client, on **Windows**. Run one exe,
connect a socket, and read and drive the game from any language.

```
┌──────────────┐    ws://127.0.0.1:24455    ┌──────────────────────────────────┐
│  your tool   │◄──────────────────────────►│  chatwire, inside the game       │
│  any lang    │   events out, commands in  │  reads it, drives it, redraws it │
└──────────────┘                            └──────────────────────────────────┘
```

No mod loader, no Forge, no JVMTI, no JNI, and nothing installed into the game directory.
chatwire is injected into the running JVM and reads HotSpot's own structures, so it works on a
client somebody else built.

```python
import asyncio, json, websockets

async def main():
    async with websockets.connect("ws://127.0.0.1:24455") as ws:
        await ws.send(json.dumps({"cmd": "net.minecraft.client.Minecraft.thePlayer"}))
        print(json.loads(await ws.recv())["result"])

asyncio.run(main())
# {'name': 'Notch', 'x': 213.5, 'y': 4.0, 'z': -520.5, 'health': 20.0, 'food': 20, ...}
```

---

## What makes it different

Four things that took the most work, and that the rest of this file assumes you have read.

### It knows which of the three 1.8.9 name sets it landed in — and proves it

The same field has three names depending on how your client was built:

```
MCP   net/minecraft/client/Minecraft . thePlayer        a dev workspace
SRG   net/minecraft/client/Minecraft . field_71439_g    an installed Forge client
OBF   ave                            . h                vanilla, what most people run
```

chatwire carries all three for every name it uses, detects which one the attached JVM speaks by
**probing a field** rather than guessing from a jar hash, and — the part that matters — checks
its own table against the client it is sitting in:

```python
{"cmd": "mapping.verify"}
-> {"mapping": "OBF (vanilla obfuscated)", "checked": 82, "missing": 0, "unchecked": 0,
    "entries": [{"group": "minecraft", "member": "the_player",
                 "spelling": "h", "kind": "field", "found": true}, …]}
```

This exists because a wrong OBF name fails **silently**: the class resolves to something real,
the member lookup finds nothing, and the feature that needed it just never works. That happened
here — `avq` shipped as GuiNewChat for a release, which is really MapItemRenderer, so every
vanilla user got a bridge that could send chat and never reported any. Nothing failed. Nothing
logged.

The table is a struct now, so C++26 static reflection walks it and asks the JVM about every
entry. Adding a name is adding a name that gets verified; there is no second list.

### It can change what the game draws

```python
{"cmd": "rewrite.add", "find": "rouge", "replace": "bleu"}   -> {"id": 1}
```

A player called `rouge_42` is now **drawn** as `bleu_42` — above their head and in the tab list
— until the rule is removed. Nothing is sent to the server; this is your client's own display.

Colours survive: chatwire hooks `ScorePlayerTeam.formatPlayerName`, the one static method every
decorated name in 1.8.9 passes through, and rewrites its *argument*, so Minecraft still applies
the team's colour and prefix. `§cchatwire_mcp§r` becomes `§cbleu_mcp§r`, not a bare string.

### Two machines, two networks

```bash
chatwire --bind 0.0.0.0 --token "correct horse battery staple"
```

The secret is never sent: the server issues a fresh 32-byte nonce and the client answers with
`HMAC-SHA256(token, nonce)`. Until that succeeds a connection can send nothing and receives no
events. `--bind` outside loopback is **refused** without a token — by the injector before it
injects, and by chatwire before a socket exists. See [Going remote](#going-remote), including
the plain statement that this is authentication and not encryption.

### It is tested against three real clients at once

[`minecrafts/`](minecrafts/) builds Minecraft 1.8.9 in **all three mappings** from Mojang's and
Forge's own published files, runs a local 1.8.9 server, joins all three clients to it, injects a
separate chatwire into each, and asks every one of them everything:

```bash
python minecrafts/mc.py all          # download, remap and build the three jars
python minecrafts/mc.py chatwire     # server + 3 clients + 3 chatwires, then ask
```

```
* vanilla on port 24455
    mapping.verify     checked=82 missing=0 unchecked=0
    loadedEntityList   3 entities: bexx2, bewx1
    tab list           3: chatwire_mcp='§cchatwire_mcp§r', …
    sendChatMessage    sent=True
    server heard it    yes
```

The same three players come back as `EntityOtherPlayerMP` on a deobfuscated client and `bex` on
a vanilla one, which is the mapping layer visibly working. There is also an offline check
(`mc.py check`) that validates the name table against the real MCP mappings without starting
anything.

---

## Quick start

Needs a compiler with **C++26 static reflection** (P2996): GCC 16 or newer. Every JSON object
chatwire puts on the wire is generated from the struct that describes it, and a compiler without
reflection is refused at configure time. MSYS2 does not ship GCC 16; the
[winlibs](https://winlibs.com) builds do.

```bash
cmake -S . -B build/etc -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_COMPILER=C:/tools/mingw64/bin/g++.exe
cmake --build build/etc
```

`build/chatwire.exe` is the only file you need — the library is carried inside it as a resource,
so there is nothing to keep together and no way to run a new injector against an old library.

```bash
chatwire                 # find Minecraft and inject
chatwire --list          # list candidate processes
chatwire --pid 1234      # pick one
chatwire --port 9000     # listen elsewhere
chatwire --console       # also open a console showing chat
```

Then connect to `ws://127.0.0.1:24455`. `pip install websockets` for the examples below.

---

## The API

Every command is the fully-qualified **Java member it reaches**, so you can check it against
Minecraft's source and the answer cannot be mistaken for something it is not. `system.*`,
`mapping.*`, `rewrite.*` and `commands.*` are the exceptions: they reach no single member, and
keeping a short name is how they say so.

A request is `{"cmd": "…", …}`; a reply is `{"ok": true, "result": {…}}` or
`{"ok": false, "error": "…"}`. Events arrive unprompted and carry `"type"` instead of `"ok"`.

### Chat

| | |
|---|---|
| `net.minecraft.client.entity.EntityPlayerSP.sendChatMessage` | say it **to the server**, as if typed. A leading `/` runs a command |
| `net.minecraft.client.entity.EntityPlayerSP.addChatMessage` | say it **only to this player**. Never transmitted |
| `net.minecraft.client.gui.GuiNewChat.printChatMessage` | *event* — every line that reached the chat box |
| `commands.register` | claim a `/name`; the player typing it never reaches the server |

```python
await ws.send(json.dumps({
    "cmd": "net.minecraft.client.entity.EntityPlayerSP.sendChatMessage",
    "text": "hello everyone"}))            # {'ok': True, 'result': {'sent': True}}
```

Max 100 characters and **plain text only** on the way out: a `§` on the wire gets the player
kicked. Incoming lines give you both spellings — `formatted` keeps the `§` codes, `plain`
strips them.

```python
CHAT = "net.minecraft.client.gui.GuiNewChat.printChatMessage"
async for raw in ws:
    event = json.loads(raw)
    if event.get("type") == CHAT:
        print(event["plain"])
```

`commands.register` claims a name and swallows it: `/ping alpha` reaches you with its arguments
split and the server never sees it. The claim is dropped when your connection closes. Also
`commands.unregister` and `commands.list`.

### Who is here — three lists that are not the same list

The commonest confusion in this area, which is why each command is the member it reads:

| | |
|---|---|
| `net.minecraft.world.World.playerEntities` | who the **client** has loaded — players near enough to exist as entities |
| `…NetHandlerPlayClient.getPlayerInfoMap` | who the **server** says is connected. The tab list, with ping |
| `net.minecraft.scoreboard.Scoreboard.getTeams` | how names are **decorated** — the prefix and suffix that colour a nametag |

```python
{"cmd": "net.minecraft.client.network.NetHandlerPlayClient.getPlayerInfoMap"}
-> {"count": 3, "players": [
     {"name": "alpha", "uuid": "…", "ping": 42,
      "display_name": "§7[VIP] alpha", "line": "§c§7[VIP] alpha§r"}, …]}
```

`line` is the **complete tab row**, asked of the method that draws it rather than assembled —
with no server-set display name the game builds the row from the team's prefix, the profile name
and the suffix, and only `line` reports the result. The tab's header and footer are
`GuiPlayerTabOverlay.header` and `.footer`.

`getTeams` gives `prefix` and `suffix` already coloured, so a player's nametag is
`prefix + name + suffix` with nothing else to ask for. `getPlayersTeam` looks one player up.

### Where you are, and what is around you

| | |
|---|---|
| `net.minecraft.client.Minecraft.thePlayer` | name, position, facing, on-ground, dimension, health, food, saturation, XP |
| `net.minecraft.client.Minecraft.currentScreen` | is a menu open, and which class it really is |
| `net.minecraft.world.World.loadedEntityList` | everything loaded — mobs, items, arrows — each with its real class |
| `…Scoreboard.getObjectiveInDisplaySlot` | the sidebar (or the tab column, or the number under a nametag) and its scores |
| `net.minecraft.client.Minecraft.loadWorld` | *event* — the client changed world. `"loaded": false` is it **leaving** one |

`thePlayer` is one command rather than eleven because the **instant** is the point: a position
and a health read a tick apart describe a player who never existed.

`loadWorld` is the only *positive* report of a disconnect — everything else is noticing the
player has gone. It fires **before** the new world is installed, so ask for the roster a moment
later rather than in the same breath.

Most sidebar rows are not players: servers build them out of fake entries whose names are the
text you see. They are reported as the game holds them.

### Changing what you see

`rewrite.add`, `rewrite.list`, `rewrite.remove`, `rewrite.clear` — see
[above](#it-can-change-what-the-game-draws). `applied` in the listing counts names actually
changed, which is what tells a rule that matches nothing from one that was never registered.

What it does not reach, because you will notice: a tab row for which the server **pushed** a
display name never goes through `formatPlayerName`. So a rule always changes nametags, and
changes tab rows on servers that leave the name alone — compare `display_name` and `line`.

A rule belongs to the connection that registered it and goes with it. A rule nobody owns is a
game quietly lying to its player with no one left to ask why.

### chatwire itself

`system.status` — version, mapping, port, clients, and **which features are actually running**.
A feature starts late when the class it hooks is not loaded yet (`GuiNewChat` does not exist
until a chat box has been drawn), so `chat` not being up at the main menu is normal and worth
waiting for rather than assuming. Also `system.stats`, `system.ping`, `system.detach`.

`mapping.verify`, `mapping.detected` (the decision **and** the four probes behind it) and
`mapping.resolve` (translate one name from any mapping into this client's spelling).

---

## Going remote

chatwire binds `127.0.0.1` and needs no authentication, which is safe for one reason: only
software already on that machine can reach it. Leaving loopback changes the question to "who may
send `sendChatMessage`", so the two move together — `--bind` outside loopback requires
`--token`.

```python
import hashlib, hmac, json, websockets

async with websockets.connect("ws://10.6.0.3:24455") as ws:
    challenge = json.loads(await ws.recv())   # {"type":"chatwire.auth.challenge","nonce":…}
    proof = hmac.new(SECRET.encode(), challenge["nonce"].encode(), hashlib.sha256).hexdigest()
    await ws.send(json.dumps({"cmd": "system.auth", "proof": proof}))
```

A wrong proof, a missing proof and a command sent too early all get the same reply and then the
connection closes, so guessing costs a TCP handshake per attempt.

### It is authenticated, not encrypted

Everything after the handshake is plaintext. The token stops someone **driving** your game; it
does nothing about someone **reading** every word you type. Across a network you do not control,
tunnel it:

```bash
chatwire --bind 10.6.0.2 --token "…"            # bind the WireGuard/Tailscale address
ssh -L 24455:127.0.0.1:24455 you@other-machine  # or leave chatwire on loopback entirely
```

chatwire does not implement TLS, and that is a refusal rather than a gap. A hand-rolled TLS
inside a DLL injected into a game — no vetted stack, no way to keep up with the next protocol
flaw — produces something that *looks* encrypted and is not, which is worse than an honest
plaintext socket behind a real tunnel.

The SHA-256 and HMAC are in `chatwire/crypto.hpp`, `constexpr`, and checked at **compile time**
against the FIPS 180-4 and RFC 4231 vectors — including the 56-byte case whose padding spills
into a second block, which is what a hand-written SHA-256 gets wrong.

---

## Examples

- **[`examples/discord_bridge.py`](examples/discord_bridge.py)** — relays chat both ways between
  a Discord channel and one *or several* chatwires, so two players on two networks see each
  other's chat. `!who` answers with each game's tab list.
- **[`mcp/`](mcp/README.md)** — an MCP server, so an AI can play.

---

## How it works

**Injection.** `chatwire.exe` carries the DLL as a resource, writes it to a temp directory named
after its own hash, and loads it with the classic `LoadLibraryW` remote thread. There is nothing
to hide from — this is your own game — and `LoadLibraryW` is the one technique the loader itself
performs, so the DLL gets a real module handle, real TLS and a real `DllMain`.

**Reaching Java.** Everything goes through [vmhook](https://github.com/xxxnpno/vmhook), which
reads HotSpot's `gHotSpotVMStructs` directly: no JVMTI agent, no JNI, no `-javaagent`. A call
into the game happens inside a `java_thread_scope`, which attaches the calling thread and enters
`_thread_in_Java` — and will not do so while a stop-the-world collection is running, so no
collection can begin inside it. That is the property a hook detour has for free.

**chatwire is not a proxy.** It runs *inside* the client and sees what the client sees. It does
not sit between you and the server, cannot read packets the client discards, and cannot show you
anything the server never sent. The scoreboard is the client's copy — a server that hides an
objective from you has hidden it from chatwire too.

**Adding a feature** is a new file and two lines in `chatwire.cpp`. A feature declares what it
is called, which command prefixes it answers to, how to start and stop, and how to handle one
command. Nothing in the server, the dispatcher or the protocol changes.

---

## Building and hacking on it

| | |
|---|---|
| `cmake --build build/etc` | build |
| `python minecrafts/mc.py all` | download and build the three 1.8.9 clients |
| `python minecrafts/mc.py check` | validate the name table against the real mappings, offline |
| `python minecrafts/mc.py chatwire` | server + three clients + three chatwires, then ask them all |
| `python minecrafts/mc.py launch mcp` | start one client on its own |

`minecrafts/` is gitignored apart from its tooling: it is ~350 MB of Mojang's and Forge's bytes,
all of it rebuildable. It pins **Mojang's own JVM** (`jre-legacy`, Java 8u51) rather than
whatever Java 8 is installed, because a JVM is not just what runs the game — it is what chatwire
reads, and different builds export different HotSpot structures.

## Licence

MIT. See [LICENSE](LICENSE).
