# chatwire

A live WebSocket API into a running **Minecraft 1.8.9** client, on **Windows**. Run one exe,
connect a socket, and read and drive the game from any language.

Built on [vmhook](https://github.com/xxxnpno/vmhook). Works on any Minecraft 1.8.9 client.

```
┌──────────────┐    ws://127.0.0.1:24455    ┌──────────────────────────────────┐
│  your tool   │◄──────────────────────────►│  chatwire, inside the game       │
│  any lang    │   events out, commands in  │  hooks chat, commands, worlds    │
└──────────────┘                            └──────────────────────────────────┘
```

## Quick start

Needs a compiler with **C++26 static reflection** (P2996), which today means **GCC 16 or newer** —
every JSON object chatwire puts on the wire is generated from the struct that describes it. MSYS2
does not ship GCC 16 yet; the [winlibs](https://winlibs.com) builds do, and are a self-contained
directory you unpack and point at.

```bash
cmake -S . -B build/etc -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_COMPILER=C:/tools/mingw64/bin/g++.exe
cmake --build build/etc
```

A compiler without reflection is refused at configure time, with the above in the error. There is no
fallback build: a second way to write the wire format is a second thing to keep correct.

`build/chatwire.exe` is the only file you need — the library is carried inside it as a resource, so
there is nothing to keep together and no way to run a new injector against an old library.

## Features

Every command is the fully-qualified **Java member it reaches**, so you can check it against
Minecraft's source. Each example below is complete and runnable — `pip install websockets`.

### `sendChatMessage` — say it to the server

Public, as if typed. A leading `/` runs a real command. Max 100 characters, and **plain text only**:
a `§` on the wire gets the player kicked.

```python
import asyncio, json, websockets

async def main():
    async with websockets.connect("ws://127.0.0.1:24455") as ws:
        await ws.send(json.dumps({
            "cmd": "net.minecraft.client.entity.EntityPlayerSP.sendChatMessage",
            "text": "hello everyone"}))
        print(json.loads(await ws.recv()))       # {'ok': True, 'result': {'sent': True}}

asyncio.run(main())
```

### `addChatMessage` — say it only to this player

Nothing is transmitted. `§a` is green.

```python
import asyncio, json, websockets

async def main():
    async with websockets.connect("ws://127.0.0.1:24455") as ws:
        await ws.send(json.dumps({
            "cmd": "net.minecraft.client.entity.EntityPlayerSP.addChatMessage",
            "text": "§athis is client-side only"}))
        print(json.loads(await ws.recv()))       # {'ok': True, 'result': {'added': True}}

asyncio.run(main())
```

### `playerEntities` — who the client has loaded

Players near enough to exist as entities. Not the server's roster.

```python
import asyncio, json, websockets

async def main():
    async with websockets.connect("ws://127.0.0.1:24455") as ws:
        await ws.send(json.dumps({"cmd": "net.minecraft.world.World.playerEntities"}))
        for who in json.loads(await ws.recv())["result"]["players"]:
            print(who["name"], who["uuid"])

asyncio.run(main())
```

### `printChatMessage` — every line in the chat box

Pushed, unprompted. Everything the **player** saw, including lines that never crossed the network.
`formatted` keeps the `§` codes, `plain` strips them.

```python
import asyncio, json, websockets

CHAT = "net.minecraft.client.gui.GuiNewChat.printChatMessage"

async def main():
    async with websockets.connect("ws://127.0.0.1:24455") as ws:
        async for raw in ws:
            event = json.loads(raw)
            if event.get("type") == CHAT:
                print(event["plain"])

asyncio.run(main())
```

### `loadWorld` — the client changed world

Pushed, unprompted. A join, a respawn, a server switch — and `"loaded": false`, which is the client
**leaving** a world: a disconnect, or a return to the title screen. It is the only positive report of
that; everything else is noticing the player has gone.

Ask for the new roster when you are told, instead of polling for it.

```python
import asyncio, json, websockets

WORLD   = "net.minecraft.client.Minecraft.loadWorld"
PLAYERS = "net.minecraft.world.World.playerEntities"

async def main():
    async with websockets.connect("ws://127.0.0.1:24455") as ws:
        async for raw in ws:
            event = json.loads(raw)
            if event.get("type") != WORLD:
                continue
            if not event["loaded"]:
                print("left the world")
                continue
            await asyncio.sleep(2)               # let the server send the players
            await ws.send(json.dumps({"cmd": PLAYERS}))

asyncio.run(main())
```

The event fires **before** the world is installed, so the players are not there yet — which is why
the roster is asked for a moment later rather than in the same breath.

### `thePlayer` — where you are and how you are doing

One command, named after the field it reads, because the **instant** is the point: a position
and a health read a tick apart describe a player who never existed.

```python
import asyncio, json, websockets

async def main():
    async with websockets.connect("ws://127.0.0.1:24455") as ws:
        await ws.send(json.dumps({"cmd": "net.minecraft.client.Minecraft.thePlayer"}))
        print(json.loads(await ws.recv())["result"])

asyncio.run(main())
```

```python
{'name': 'chatwire', 'x': 213.5, 'y': 4.0, 'z': -520.5, 'yaw': 0.0, 'pitch': 0.0,
 'on_ground': True, 'dimension': 0, 'health': 20.0, 'food': 20,
 'saturation': 5.0, 'experience_level': 0}
```

`dimension` is Minecraft's number — 0 overworld, −1 nether, 1 end. `health` and `food` are out
of 20, as the HUD shows them. Fails with `not in a world` at the title screen.

### `currentScreen` — is a menu open, and which

```python
{"cmd": "net.minecraft.client.Minecraft.currentScreen"}
-> {"open": true, "screen": "net/minecraft/client/gui/GuiIngameMenu"}
```

`screen` is the class the screen **really** is, in the attached client's own spelling: that on a
deobfuscated client, `axs` on a vanilla one. Both are the honest answer for that jar, and
`mapping.resolve` relates them. `open` is false in the world, and then `screen` is `""` — which
is the answer a client wants before pretending to type.

### `commands.register` — add a command to the game

Claim a name and the player typing it never reaches the server: chatwire swallows the line and
pushes it to you, arguments split. The claim is dropped when the connection closes.

```python
import asyncio, json, websockets

TYPED = "net.minecraft.client.entity.EntityPlayerSP.sendChatMessage"
ADD   = "net.minecraft.client.entity.EntityPlayerSP.addChatMessage"

async def main():
    async with websockets.connect("ws://127.0.0.1:24455") as ws:
        await ws.send(json.dumps({"cmd": "commands.register", "name": "ping"}))

        async for raw in ws:
            event = json.loads(raw)
            if event.get("type") == TYPED and event.get("command") == "ping":
                who = event["args"][0] if event["args"] else "world"
                await ws.send(json.dumps({"cmd": ADD, "text": f"§apong, {who}"}))

asyncio.run(main())
```

`/ping alpha` prints `pong, alpha`. Also `commands.unregister` and `commands.list`.

`sendChatMessage` is intercepted too, so do not name a plugin's output after a command it claimed.

### `system.*` — chatwire itself

```python
import asyncio, json, websockets

async def main():
    async with websockets.connect("ws://127.0.0.1:24455") as ws:
        for verb in ("status", "stats", "ping"):
            await ws.send(json.dumps({"cmd": f"system.{verb}"}))
            print(verb, json.loads(await ws.recv())["result"])

asyncio.run(main())
```

`system.detach` stops chatwire; the connection then closes.

### `mapping.*` — is chatwire's name table right for *this* client?

The same field in 1.8.9 has three names depending on how your client was built, and chatwire
carries all three for every name it uses. A wrong one fails **silently** — the lookup finds
nothing and the feature that needed it simply never works. `mapping.verify` asks the JVM about
every entry in one request, so that failure is one command away instead of invisible.

```python
import asyncio, json, websockets

async def main():
    async with websockets.connect("ws://127.0.0.1:24455") as ws:
        await ws.send(json.dumps({"cmd": "mapping.verify"}))
        r = json.loads(await ws.recv())["result"]
        print(r["mapping"], r["checked"], "names,", r["missing"], "missing")
        for e in r["entries"]:
            if not e["found"]:
                print("  ABSENT", e["group"], e["member"], e["spelling"])

asyncio.run(main())
```

```
OBF (vanilla obfuscated) 24 names, 0 missing
```

`mapping.detected` returns the decision **and the four probes it was made from**, which is the
whole diagnosis when a client comes back `unknown`:

```json
{"mapping":"OBF (vanilla obfuscated)","minecraft_class":false,
 "mcp_field":false,"srg_field":false,"obf_class":true}
```

`mapping.resolve` translates one name into this client's spelling. It takes any of the four
things you might have — `thePlayer`, `field_71439_g`, `h`, or chatwire's own `the_player` —
and `minecraft.the_player` when the short form is ambiguous:

```json
{"cmd": "mapping.resolve", "name": "printChatMessage"}
-> {"group":"gui_new_chat","member":"print_chat_message",
    "mcp":"printChatMessage","srg":"func_146227_a","obf":"a",
    "spelling":"a","kind":"field and method","found":true}
```

`kind` is what the JVM turned out to have. "field and method" is normal on a vanilla client:
obfuscation reuses one letter across kinds, so `a` is both.

The table walks itself — `mapping::table` is a struct and the verifier reflects over it — so a
name added to chatwire is a name that gets verified, with no second list to keep in step.
[minecrafts/](minecrafts/) builds all three clients so this can be checked against every one of
them at once.

## An AI in the game

See [mcp/README.md](mcp/README.md).

## Protocol

Every message is one flat JSON object. A command is `<class>.<member>`, split at the **last** dot.

**Sent by you:**

| Command | Arguments | Effect |
|---|---|---|
| `net.minecraft.client.entity.EntityPlayerSP.sendChatMessage` | `text` (≤100) | To the server, as if typed |
| `net.minecraft.client.entity.EntityPlayerSP.addChatMessage` | `text` | Client-side only |
| `net.minecraft.world.World.playerEntities` | — | Players the client has loaded |
| `commands.register` / `.unregister` | `name` | Claim `/name` in the game, or give it back |
| `commands.list` | — | Claimed commands and their owners |
| `system.status` / `.stats` / `.ping` | — | chatwire's own state |
| `system.detach` | — | Unloads chatwire |

Every command gets a reply:

```json
{"ok":true,"result":{"sent":true}}
{"ok":false,"error":"'text' exceeds the 100-character chat limit"}
```

`sendChatMessage` and `addChatMessage` are **synchronous**: `sent`/`added` means the call into
Minecraft has already happened. Not in a world gives `{"ok":false,"error":"not in a world"}`.

**Pushed to you:**

```json
{"type":"net.minecraft.client.gui.GuiNewChat.printChatMessage",
 "formatted":"§b[Team] §fhi","plain":"[Team] hi"}

{"type":"net.minecraft.client.entity.EntityPlayerSP.sendChatMessage",
 "command":"ping","args":["alpha"],"raw":"/ping alpha"}

{"type":"net.minecraft.client.Minecraft.loadWorld","loaded":true}
```

An event is named after the method it comes **out of**. The second shares its name with a command
you can send; that is not a clash — `type` is what happened, `cmd` is what you are asking for.

`loadWorld` fires on every world change, `loaded` false being the client leaving one. It arrives
**before** the new world is installed, so treat it as "ask again now", not as a description of where
the player is.

`system` and `commands` are the only short prefixes. They reach nothing in the game, so there is no
Java member to name them after and nothing for a reader to check.

## chatwire is not a proxy

A proxy (prismarine, `node-minecraft-protocol`, a server plugin) speaks the network protocol and
can only see what was **transmitted**. Mod output, client-side command replies and warnings the
client generated never cross the network, so no proxy can observe them. chatwire hooks the method
that *renders* chat and sees both.

Use a proxy for headless bots, multi-version support, or anything that is not Windows.

## Licence

MIT.
