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

The complete protocol — every command, every field, every event — is
[below](#the-protocol). There are 27 commands and 4 events, and nothing else on the wire.

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

### It can change what the game draws — on all four surfaces, or stop it drawing at all

```python
{"cmd": "rewrite.add", "find": "rouge", "replace": "bleu"}   -> {"id": 1}
```

A player called `rouge_42` is now **drawn** as `bleu_42` — above their head, in the tab list, in
the sidebar and in chat — until the rule is removed. Nothing is sent to the server; this is your
client's own display.

Those four surfaces do not share a code path, so they are three hooks and not one:

```
tab list      GuiPlayerTabOverlay → ScorePlayerTeam.formatPlayerName
sidebar       GuiIngame           → ScorePlayerTeam.formatPlayerName
above a head  Render.renderLivingLabel, with the text ALREADY BUILT
chat          GuiNewChat.printChatMessage, a whole IChatComponent
```

Rewriting only the team formatter — which is what this did first — changed the tab and the
sidebar and left the name over the player's head alone, which looks exactly like a rule that did
not match.

Colours survive on nametags: chatwire rewrites `formatPlayerName`'s **argument**, so Minecraft
still applies the team's colour and prefix. `§cchatwire_mcp§r` becomes `§cbleu_mcp§r`, not a bare
string.

A rule can also say **do not draw this at all**:

```python
{"cmd": "rewrite.add", "match": "SPAMBOT", "drop": True}
```

and no chat line and no floating nametag carrying that text is drawn again until the rule goes —
the hook cancels the method before its body runs, so there is nothing to suppress afterwards.
See [rewrite](#rewrite--what-the-game-draws) for what each hook hands a rule (not the same
string), what a drop can and cannot reach, and what rewriting a chat line costs.

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

Needs **GCC 16.2 or newer** and **CMake 3.28+**. chatwire is written as C++26 **modules, all the
way down** — 29 `.ixx` module interface units of its own plus vendored vmhook, **no headers and
no `.cpp` at all**, with the standard library arriving as `import std;`. The preprocessor survives
in 23 lines across 10 files, every one of them an `#include` in a global module fragment: the
Win32 headers, where Win32 has nowhere else to live, and one `<cstdio>` for the three stream
macros a module cannot export. There is no `#define`, `#ifdef`, `#if` or `#pragma` anywhere. It
also needs **static reflection** (P2996): every JSON object on the wire is generated
from the struct that describes it, and a compiler without it is refused at configure time. MSYS2
does not ship GCC 16; the [winlibs](https://winlibs.com) builds do.

```bash
cmake -S . -B build/etc -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_COMPILER=C:/tools/mingw64/bin/g++.exe
cmake --build build/etc
```

The build compiles libstdc++'s own `bits/std.cc` into a `chatwire_std` target, because nothing
else does: `import std;` without a compiled `std.gcm` fails with *"failed to read compiled
module"*, which reads like a broken install and is not one. CMake can do this itself via
`CXX_MODULE_STD`, but only behind an experimental gate whose value is a UUID that changes every
release, and a build that breaks when you upgrade CMake is worse than eight explicit lines.

It also links with `-Wl,--allow-multiple-definition`, working around a GCC 16.2 bug rather than
by choice: a function-local `static` in an **inline** function in a module's purview is emitted
into the module's object *and* every consumer's, and letting the linker pick one of two copies is
fine for code but silently splits state. So the rule in this codebase is that any function
holding state is **not inline** — a non-inline definition in an interface unit is emitted exactly
once. That is what let four `*_impl.cpp` files disappear rather than being renamed.

`build/chatwire.exe` is the only file you need — the library is carried inside it as a resource,
so there is nothing to keep together and no way to run a new injector against an old library.

| | |
|---|---|
| `chatwire` | find Minecraft and inject |
| `chatwire --list` | list candidate processes and exit |
| `chatwire --pid 1234` | pick one |
| `chatwire --port 9000` | listen elsewhere (default 24455) |
| `chatwire --bind 10.6.0.2` | listen off loopback — **requires `--token`** |
| `chatwire --token "…"` | require `system.auth` before anything else |
| `chatwire --console` | also open a console showing chat |
| `chatwire --background` | no console (the default) |
| `chatwire --verbose` | more log |
| `chatwire --dll <path>` | load that library instead of the built-in copy |

Then connect to `ws://127.0.0.1:24455`. `pip install websockets` for the examples below.

---

## The protocol

One WebSocket, JSON text frames both ways. Nothing is chunked, nothing is binary, and there is
no subprotocol to negotiate — an ordinary `ws://` handshake, then frames.

**A request** is an object with `cmd` and whatever that command needs:

```json
{"cmd": "net.minecraft.client.entity.EntityPlayerSP.sendChatMessage", "text": "hello"}
```

**A reply** comes back on the same socket, in order, one per request:

```json
{"ok": true,  "result": {"sent": true}}
{"ok": false, "error": "not in a world"}
```

**An event** arrives unprompted and carries `"type"` instead of `"ok"`. `type` is what happened
in the game; `cmd` is what you are asking for; they are different keys and an event never has an
`ok`.

Every command that reaches into the game is spelled as the **fully-qualified Java member it
reaches**, so you can check it against Minecraft's source and the answer cannot be mistaken for
something it is not. `system.*`, `mapping.*`, `rewrite.*` and `commands.*` are the exceptions:
they reach no single member, and keeping a short name is how they say so. The names are always
the **deobfuscated** ones whatever mapping the client is in — you should not have to know how
somebody else built their jar to write a client.

A `cmd` splits at its **last** dot: everything before it is the feature's prefix (normally a
fully-qualified class, dots and all) and everything after is the member. WebSocket `ping` is
answered with a `pong`, `close` is answered and honoured, and fragmented text frames are
reassembled before anything reads them.

### Authentication

With no `--token`, a connection is authenticated the moment it is open and this section does not
apply. With one, the server sends a challenge **before reading anything** and the connection can
do nothing until it answers:

```
server → {"type": "chatwire.auth.challenge", "nonce": "<64 hex chars>"}
client → {"cmd": "system.auth", "proof": "<hex HMAC-SHA256(token, nonce)>"}
server → {"ok": true, "result": {"authenticated": true}}
```

A wrong proof, a missing proof, and any other command sent first all get the **same** reply and
then the socket closes:

```json
{"ok": false, "error": "authenticate first: send {\"cmd\":\"system.auth\",\"proof\":\"<hmac>\"}"}
```

so guessing costs a TCP handshake per attempt and learns nothing about which part was wrong.

### Chat

`chatwire.features.chat` — hooks `GuiNewChat.printChatMessage`, calls `EntityPlayerSP`.

| cmd | body | result |
|---|---|---|
| `net.minecraft.client.entity.EntityPlayerSP.sendChatMessage` | `text` | `{"sent": true}` |
| `net.minecraft.client.entity.EntityPlayerSP.addChatMessage` | `text` | `{"added": true}` |

`sendChatMessage` goes **to the server**, exactly as if typed — a leading `/` runs a command and
other players see it. `addChatMessage` is **client-side only** and is never transmitted. That
distinction is the single most important thing about this API and the easiest to get wrong,
which is why they are two commands and not a flag.

Max 100 characters on `sendChatMessage` (1.8.9 kicks the player for more, so this refuses first)
and **plain text only** on the way out: a `§` on the wire gets the player kicked. Both fail with
`not in a world` at the title screen rather than dropping the line silently.

```python
await ws.send(json.dumps({
    "cmd": "net.minecraft.client.entity.EntityPlayerSP.sendChatMessage",
    "text": "hello everyone"}))            # {'ok': True, 'result': {'sent': True}}
```

### Who is here — three lists that are not the same list

`chatwire.features.world`, `chatwire.features.scoreboard`. The commonest confusion in this area,
which is why each command is the member it reads:

| cmd | body | result |
|---|---|---|
| `net.minecraft.world.World.playerEntities` | — | `{"count", "players": [{"name", "uuid"}]}` |
| `net.minecraft.client.network.NetHandlerPlayClient.getPlayerInfoMap` | — | `{"count", "players": [{"name", "uuid", "ping", "display_name", "line"}]}` |
| `net.minecraft.scoreboard.Scoreboard.getTeams` | — | `{"count", "teams": [{"name", "display_name", "prefix", "suffix", "members"}]}` |
| `net.minecraft.scoreboard.Scoreboard.getPlayersTeam` | `name` | one team, same shape |

`playerEntities` is who the **client** has loaded — players near enough to exist as entities.
`getPlayerInfoMap` is who the **server** says is connected: the tab list, with ping.
`getTeams` is how names are **decorated** — `prefix + name + suffix` is a player's nametag with
nothing else to ask for.

```python
{"cmd": "net.minecraft.client.network.NetHandlerPlayClient.getPlayerInfoMap"}
-> {"count": 3, "players": [
     {"name": "alpha", "uuid": "…", "ping": 42,
      "display_name": "§7[VIP] alpha", "line": "§c§7[VIP] alpha§r"}, …]}
```

`line` is the **complete tab row**, asked of the method that draws it rather than assembled —
with no server-set display name the game builds the row from the team's prefix, the profile name
and the suffix, and only `line` reports the result.

| cmd | body | result |
|---|---|---|
| `net.minecraft.client.gui.GuiPlayerTabOverlay.header` | — | `{"text"}` |
| `net.minecraft.client.gui.GuiPlayerTabOverlay.footer` | — | `{"text"}` |

`""` is the normal answer to both: most servers set neither. They are their own commands because
a packet pushes them straight into those two fields and the roster command cannot report them.

### Where you are, and what is around you

| cmd | body | result |
|---|---|---|
| `net.minecraft.client.Minecraft.thePlayer` | — | `{"name", "x", "y", "z", "yaw", "pitch", "on_ground", "dimension", "health", "food", "saturation", "experience_level"}` |
| `net.minecraft.client.Minecraft.currentScreen` | — | `{"open", "screen"}` |
| `net.minecraft.world.World.loadedEntityList` | — | `{"count", "entities": [{"id", "type", "name", "custom_name", "display_name", "x", "y", "z", …}]}` |
| `net.minecraft.scoreboard.Scoreboard.getObjectiveInDisplaySlot` | `slot` | `{"slot", "name", "display_name", "scores": [{"name", "points"}]}` |

`thePlayer` is one command rather than eleven because the **instant** is the point: a position
and a health read a tick apart describe a player who never existed.

`currentScreen` answers in or out of a world — "no screen is open" is a real answer at the title
screen too, and a client polling this to know when it may type should not have to special-case
the one moment it most wants to watch. `screen` is the class it really is, in this jar's
spelling.

`entities` carries every loaded entity — mobs, items, arrows — each with its real class in
`type`, its `custom_name` (an anvil or `/summon` name, `""` for almost everything) and the
`display_name` the game actually **draws** above it.

`slot` is `list`, `sidebar`, `belowName`, or `"0"`–`"2"`; it defaults to `sidebar`, and the reply
names it back in words. Most sidebar rows are not players: servers build them out of fake entries
whose names are the text you see, and they are reported as the game holds them rather than
filtered, because deciding which are real is the caller's business.

`playerEntities`, `loadedEntityList` and `thePlayer` all answer `not in a world` when there
isn't one.

### rewrite — what the game draws

`chatwire.features.rewrite`. Rules are held in one set and applied, in registration order, to
**every** name and chat line the game is about to draw, so a later rule sees what an earlier one
produced.

| cmd | body | result |
|---|---|---|
| `rewrite.add` | `match`?, and one of `template`, `find`+`replace`, or `drop` | `{"id"}` |
| `rewrite.remove` | `id` (as a string) | `{"removed"}` |
| `rewrite.clear` | — | `{"removed"}` |
| `rewrite.list` | — | `{"count", "applied", "dropped", "rules": [{"id", "match", "pattern", "find", "replace", "drop", "owner"}]}` |

- **`match`** — *whose* text this rule is for. A **substring** test, not equality, and an absent
  or empty one means every name and every line. It is a substring test because each hook hands a
  rule a different string: `formatPlayerName` gets the raw name, `renderLivingLabel` gets the
  finished label with its colours in, and chat gets the whole line — so a rule about `alpha`
  sees `alpha` in one and `§calpha§r` in another.
- **`find` + `replace`** — plain substitution, every occurrence. The shortest way to say "call
  them something else". `find` may not be empty and needs a `replace`.
- **`template`** — replaces the whole string instead of editing it, with placeholders resolved
  at draw time: `{name}`, `{health}`, `{food}`, `{ping}`, `{x}`, `{y}`, `{z}`. A placeholder that
  cannot be filled yet — the snapshot has not caught up, or that name is not a player — is
  omitted rather than printed, so a nametag never reads `alpha {health}` at somebody.
- **`drop`** — do not draw it at all. See [below](#hiding-something-instead-of-changing-it).

```python
{"cmd": "rewrite.add", "match": "alpha", "template": "{name} §c{health}❤"}
{"cmd": "rewrite.add", "find": "§k", "replace": ""}          # every line, everywhere
{"cmd": "rewrite.add", "match": "SPAMBOT", "drop": True}     # and never see it again
```

64 rules at most. `applied` in the listing counts names actually changed since chatwire started,
which is what tells a rule that matches nothing from one that was never registered; `dropped` is
the same number for the rules that hide.

#### Hiding something instead of changing it

```python
{"cmd": "rewrite.add", "match": "Guild >", "drop": True}   -> {"id": 3}
```

From that moment no chat line containing `Guild >` is drawn: the line reaches the client, the
hook cancels `printChatMessage` before its body runs, and nothing is built, stored or drawn. It
is the cheapest thing a rule can do — there is no replacement to allocate and no line to
re-issue.

A drop rule takes a `match` and **nothing else** — no `find`, no `template`, no `replace` — and
that `match` may not be empty. Everywhere else in this feature an empty match means "everyone",
which is a loud and reversible mistake; a drop with no match would hide every chat line and every
nametag in the game at once, and from the inside that looks exactly like a client that has
silently stopped working. It is refused rather than made possible to type by accident.

**A drop wins.** If several rules match, the first `drop` among them ends the matter — rules
registered after it do not run, and a rewrite registered before it is discarded. Text that is not
going to be drawn is not worth editing, and answering "hidden, and also here is the new text"
would leave two callers to decide which half they believed.

**What each surface can do about it**, because they are three hooks and only two of them return
nothing:

| surface | what `drop` does |
|---|---|
| chat line | not drawn at all — `printChatMessage` returns void, so cancelling it is the whole answer |
| floating nametag | not drawn at all — same, on `renderLivingLabel` |
| tab row / sidebar row | the **name is emptied**, leaving the team's prefix and suffix |

The last one is the honest limit rather than an oversight: a tab row is drawn from what
`formatPlayerName` *returns*, so there is no not-drawing it from inside — cancelling would hand
Minecraft a null String and it would draw an exception instead of a row.

**A hidden line still reaches the socket**, marked `"dropped": true`, and still shows in
chatwire's own `--console`. Hiding is about the player's screen: a tool that hides a spammer has
every reason to still count it, a bridge relaying chat elsewhere should not go quiet because
somebody stopped wanting to *read* the lines, and the one window that could explain where the
chat went is the worst possible place to hide it.

**What it does not reach, because you will notice.** A tab row for which the server **pushed** a
display name never goes through `formatPlayerName`, so a rule always changes nametags and changes
tab rows on servers that leave the name alone — compare `display_name` and `line`.

**What rewriting a chat line costs.** A line cannot be edited in place: handing the detour's
argument a fresh component crashes the JVM outright, because `printChatMessage` *stores* what it
is given. So a matched line is **cancelled and re-issued** as a plain `ChatComponentText`
carrying the rewritten text. The `§` codes in that text still colour it, but the original
component's structure is gone with it — **click and hover events do not survive a rewrite**, so
a rewritten line with a URL in it is no longer clickable. A rule that matches nothing costs
nothing and changes nothing, which is the overwhelmingly common case.

A rule belongs to the connection that registered it and goes with it; `rewrite.clear` clears only
your own, so one tool cannot silently undo another's. A rule nobody owns is a game quietly lying
to its player with no one left to ask why.

### commands — `/name` added to the game at runtime

`chatwire.features.commands`.

| cmd | body | result |
|---|---|---|
| `commands.register` | `name` | `{"registered"}` |
| `commands.unregister` | `name` | `{"unregistered"}` |
| `commands.list` | — | `{"count", "commands": [{"name", "client"}]}` |

`commands.register` claims a name and swallows it: from that moment `/ping alpha` is intercepted
before the packet is built, **the server never sees it**, and the event goes to the one client
that registered it — not to everybody, because two plugins claiming `/ping` would otherwise both
act on it. 256 registrations at most. The claim is dropped when your connection closes, so a
plugin that crashes cannot leave `/ping` swallowed forever with nobody left to answer.

That round trip *is* the plugin system: there is no plugin format and nothing to compile against.

### mapping — chatwire checking itself

`chatwire.features.mapping`.

| cmd | body | result |
|---|---|---|
| `mapping.detected` | — | `{"mapping", "minecraft_class", "mcp_field", "srg_field", "obf_class"}` |
| `mapping.verify` | — | `{"mapping", "checked", "missing", "unchecked", "entries": [{"group", "member", "spelling", "kind", "found"}]}` |
| `mapping.resolve` | `name` | `{"group", "member", "mcp", "srg", "obf", "spelling", "kind", "found"}` |

`detected` reports the decision **and** the four probes behind it. `verify` asks the JVM about
every entry in the table — `missing` and `unchecked` are different failures and only the first is
one. `resolve` translates one name from any mapping into this client's spelling, and only for
names chatwire's own table carries.

### system — chatwire itself

`chatwire.features.system`.

| cmd | body | result |
|---|---|---|
| `system.ping` | — | `{"pong": true}` |
| `system.status` | — | `{"version", "mapping", "port", "clients", "can_call", "features": [{"name", "started"}]}` |
| `system.stats` | — | every counter, below |
| `system.detach` | — | `{"detaching": true}` |
| `system.auth` | `proof` | `{"authenticated": true}` |

`features` is not decoration. A feature starts **late** when the class it hooks is not loaded yet
— `GuiNewChat` does not exist until a chat box has been drawn — so `chat` not being up at the
main menu is normal, and a client that wants chat should wait for this to say so rather than
assume. `can_call` is whether vmhook will let this JVM be called into at all.

`system.stats` is every counter-keeping feature's numbers flattened into one object:

```
lines_seen  sent  added                        chat
commands_run  commands_dropped                 commands
player_queries  worlds_entered  worlds_left  player_states  entity_queries    world
scoreboard_queries  team_queries  tab_queries  scoreboard
rewrites_applied  rewrites_dropped  rewrite_rules   rewrite
```

`system.detach` unhooks everything and unloads the library; the reply is sent first, and the
teardown runs on a thread of its own because the connection asking is one of the threads being
joined.

### Events — the four things that arrive unprompted

| type | who gets it | body |
|---|---|---|
| `chatwire.auth.challenge` | the connection, before its first frame is read, only with `--token` | `nonce` |
| `net.minecraft.client.gui.GuiNewChat.printChatMessage` | every authenticated client | `formatted`, `plain`, `dropped` |
| `net.minecraft.client.Minecraft.loadWorld` | every authenticated client | `loaded` |
| `net.minecraft.client.entity.EntityPlayerSP.sendChatMessage` | **only** the client that registered that command | `command`, `args`, `raw` |

**`printChatMessage`** is every line that reached the chat box — server messages, client
messages, mod output, death messages, join and leave — because one hook on that one method
catches all of them. `formatted` keeps the `§` codes and `plain` strips them; you get both
because guessing which one a caller wanted is how a filter ends up matching against colour codes
by accident. `dropped` is whether a `rewrite` rule hid this line from the player, who did not see
what you are reading.

```python
CHAT = "net.minecraft.client.gui.GuiNewChat.printChatMessage"
async for raw in ws:
    event = json.loads(raw)
    if event.get("type") == CHAT:
        print(event["plain"])
```

**`loadWorld`** is the client changing world; `"loaded": false` is it **leaving** one, and it is
the only *positive* report of a disconnect — everything else is noticing the player has gone. It
fires **before** the new world is installed, so ask for the roster a moment later rather than in
the same breath.

**`sendChatMessage`** as an event is a `/command` you claimed, arriving with its arguments
already split:

```json
{"type": "net.minecraft.client.entity.EntityPlayerSP.sendChatMessage",
 "command": "ping", "args": ["alpha", "beta"], "raw": "/ping alpha beta"}
```

It shares its name with a command you can send, and that is not a collision: `type` is what
happened in the game, `cmd` is what you are asking for.

### Errors

`{"ok": false, "error": "…"}`, and the text is meant to be read by a person:

```
missing or non-string 'cmd'
'cmd' must look like <class>.<member>, e.g. net.minecraft.world.World.playerEntities
no feature named 'net.minecraft.client.Whatever'
not in a world
missing or non-string 'text'
'text' exceeds the 100-character chat limit
unknown member; try sendChatMessage or addChatMessage
give either 'template' (with {name}, {health}, {food}, {ping}, {x}, {y}, {z}), or 'find' and 'replace', or 'drop' with a 'match'
'drop' needs a non-empty 'match' -- a rule that hides everything is never what anyone means
'drop' takes a 'match' and nothing else -- a rule that hides something has nothing to draw instead
too many rules; remove some first
'slot' must be list, sidebar, belowName, or 0-2
no entry with that name; chatwire's table only carries the names it uses
```

A malformed frame, an unknown prefix and an unknown member are all errors on the socket rather
than a closed connection — the only thing that closes a connection by itself is a failed
authentication.

---

## Building a QoL client on top

chatwire is not a mod and does not want to be one, but the loop most client-side quality-of-life
mods are actually made of — *watch chat, decide something, say something, and draw something
else* — is exactly what the four events and 27 commands add up to. If you want to build the sort
of thing [npnoqol](https://github.com/xxxnpno) is, here is what maps onto what, and what does
not, said plainly so you find out now rather than three modules in.

| what you want | how |
|---|---|
| `/somecommand` that the server never sees | `commands.register`, then the `sendChatMessage` **event** |
| answering it in the player's own chat | `addChatMessage` — client-side, nothing transmitted |
| reacting to a chat line (auto-GG, greeting a guild member, booping a friend) | the `printChatMessage` event → your own throttle → `sendChatMessage` |
| an API call in the middle of that (stats, denick, claiming a reward link) | in **your** program, in whatever language it is written in. chatwire is not in the way |
| stripping obfuscation (`§k`) out of every line | `rewrite.add` with `find: "§k"`, `replace: ""` |
| never seeing a spammer, a bot, or a whole category of message again | `rewrite.add` with a `match` and `drop` |
| showing health, ping or distance above a player's head | `rewrite.add` with a `template` |
| knowing which game mode you are in | `getObjectiveInDisplaySlot`, plus the chat lines you already have |
| knowing when you joined or left | the `loadWorld` event |

The throttling, the caching, the "only greet each person once", the API keys and the config file
all live in your program, which is the point: they are ordinary code in a language with libraries,
not something chatwire has to grow a verb for.

**What is out of reach, and will stay that way.** Anything that is not chat, a name, or a value
you can read:

- **You cannot change the game's behaviour.** No render hooks, no input hooks, no physics — a
  camera that clips through walls, a reach change, a particle filter, all of that means hooking
  `EntityRenderer` or `World.rayTraceBlocks` yourself, and chatwire has no verb that reaches
  them and is not going to grow one per idea.
- **You cannot intercept ordinary chat on its way out.** Only `/names` you registered are
  swallowed; anything else the player types goes to the server untouched.
- **You cannot read what the client did not receive.** chatwire runs *inside* the client and is
  not a proxy: a packet the client discarded is gone, and an objective the server hid from you is
  hidden from chatwire too.

The last two are the same boundary seen from two sides — chatwire owns the wire, and what you do
with what comes off it is your program's business. If you need the first kind of thing, you want
a mod, and [vmhook](https://github.com/xxxnpno/vmhook) is what chatwire itself is built on.

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

The SHA-256 and HMAC are in `chatwire/crypto.ixx`, `constexpr`, and checked at **compile time**
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

**Answers are synchronous.** A command runs on the socket thread that received it and calls into
Java from there, so a reply says what actually happened rather than "accepted". There is no queue
and no tick to wait for: `{"sent": true}` means the game has already sent it.

**chatwire is not a proxy.** It runs *inside* the client and sees what the client sees. It does
not sit between you and the server, cannot read packets the client discards, and cannot show you
anything the server never sent. The scoreboard is the client's copy — a server that hides an
objective from you has hidden it from chatwire too.

**Adding a feature** is a new module and two lines in `chatwire.ixx`. A feature declares what it
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
