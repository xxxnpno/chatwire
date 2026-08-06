# chatwire

A live WebSocket API into a running **Minecraft 1.8.9** client, on **Windows**. Run one exe,
connect a socket, and read and drive the game from any language.

C++23, built on [vmhook](https://github.com/xxxnpno/vmhook): HotSpot introspection, no JVMTI, no
mod loader, minimal JNI. Handles all three 1.8.9 mappings (MCP/SRG/OBF), so it attaches to vanilla,
Forge and Lunar the same way — and to whatever JVM they run on, Java 8 through 26. It attaches to a
game that is **already running**, which is what makes it Windows-only: that needs
`CreateRemoteThread` + `LoadLibrary`.

```
┌──────────────┐    ws://127.0.0.1:24455    ┌──────────────────────────────────┐
│  your tool   │◄──────────────────────────►│  chatwire, inside the game       │
│  any lang    │   events out, commands in  │  hooks chat in and chat out      │
└──────────────┘                            └──────────────────────────────────┘
```

## Quick start

```bash
cmake -S . -B build/etc -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/etc
```

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
```

An event is named after the method it comes **out of**. The second shares its name with a command
you can send; that is not a clash — `type` is what happened, `cmd` is what you are asking for.

`system` and `commands` are the only short prefixes. They reach nothing in the game, so there is no
Java member to name them after and nothing for a reader to check.

## chatwire is not a proxy

A proxy (prismarine, `node-minecraft-protocol`, a server plugin) speaks the network protocol and
can only see what was **transmitted**. Mod output, client-side command replies and warnings the
client generated never cross the network, so no proxy can observe them. chatwire hooks the method
that *renders* chat and sees both.

Use a proxy for headless bots, multi-version support, or anything that is not Windows.

## Design notes

**Any thread can call Java.** vmhook enters via pure VMStructs where the VM publishes a usable
polling page (Java 8–20) and minimal JNI everywhere else, including Java 17. There is no pump and
no tick to wait for, so commands are synchronous and report what happened.

**What the VM permits is not what Minecraft permits.** `sendChatMessage` is genuinely thread-safe —
it hands off to the netty event loop. `addChatMessage` races cosmetically: worst case, one line
drawn twice for a frame.

**It runs inside someone's game.** No exception reaches the JVM. Nothing blocks the game thread.
`system.detach` stops chatwire but leaves the module mapped — unloading while a game thread might
be inside a trampoline produced a DEP violation on Minecraft's own thread, twice.

**Swallowing a chat message needs a reason; letting one through needs none.** The command
interceptor is the only hook that changes what the game does, and every failure in it ends with the
player's line going to the server.

**Layering.** `sdk.hpp` is the only header that includes vmhook; `net.hpp` the only one that
includes Winsock; `chatwire.hpp` neither.

## Security

**Binds `127.0.0.1` only, and that is not configurable.** This socket sends chat as the player and
reads everything they see. There is no authentication, and that is only defensible because of the
bind address.

Chat text is attacker-controlled and flows into the JSON chatwire emits, so output is escaped.

## Requirements

**Windows**, x86-64, HotSpot JVM. **CMake 3.20+** and GCC 14+ (developed against GCC 15.2 via
MSYS2) or MSVC 19.37+ — C++23 with a working `<print>`. No package manager; `vmhook` is vendored
in `ext/` and the only library linked is `ws2_32`.

| Option | |
|---|---|
| `-DCHATWIRE_BUILD_TOOLS=OFF` | just the library; drops the `<print>` requirement |
| `-DCHATWIRE_EMBED_DLL=OFF` | ship the exe and the DLL side by side instead of embedding |

## Status

Works end to end, verified by injecting into a real Minecraft. There is no CI: everything that
matters here needs a live game.

## Licence

MIT.
