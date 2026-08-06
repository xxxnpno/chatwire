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

Every command is the fully-qualified **Java member it reaches**, so you can check what you are
getting against Minecraft's source rather than trusting a summary. Bind them once:

```python
import asyncio, json, websockets                # pip install websockets

PLAYER  = "net.minecraft.client.entity.EntityPlayerSP."
SEND    = PLAYER + "sendChatMessage"
ADD     = PLAYER + "addChatMessage"
PLAYERS = "net.minecraft.world.World.playerEntities"
CHAT    = "net.minecraft.client.gui.GuiNewChat.printChatMessage"

async def call(ws, cmd, **args):
    await ws.send(json.dumps({"cmd": cmd, **args}))
    reply = json.loads(await ws.recv())
    if not reply["ok"]:
        raise RuntimeError(reply["error"])
    return reply["result"]
```

### `sendChatMessage` — say it to the server

Public. Other players see it, and a leading `/` runs a real command. Refused over 100 characters.

```python
await call(ws, SEND, text="hello everyone")            # {'sent': True}
```

**Plain text only.** A `§` colour code here goes out on the wire, where a vanilla server treats it
as an illegal chat character and kicks the player. Colours are for `addChatMessage`.

### `addChatMessage` — say it only to this player

Nothing is transmitted. Draws a line in the local chat box and nothing more. `§a` is green.

```python
await call(ws, ADD, text="§athis is client-side only")  # {'added': True}
```

These two are the easiest thing in the API to confuse, which is why they are separate commands
rather than one with a flag.

### `playerEntities` — who the client has loaded

Name and UUID come from the same object in one pass, so an entry's halves always belong together.

```python
result = await call(ws, PLAYERS)
for who in result["players"]:
    print(who["name"], who["uuid"])
```

Not the server's roster: these are the players near enough to exist as entities, which on a big
server is a small fraction of the tab list. The command is the field it reads for that reason.

### `printChatMessage` — every line in the chat box

Server messages, mod output, client-side replies, death messages. One hook catches all of it, so
this is what the **player** saw, not what the network carried.

```python
async for raw in ws:
    event = json.loads(raw)
    if event.get("type") == CHAT:
        print(event["plain"])          # "formatted" keeps the § colour codes
```

### `commands.register` — add a command to the game

Claim a name. From then on the player typing it does not reach the server: chatwire swallows the
line and pushes it to you with the arguments split.

```python
async def plugin():
    async with websockets.connect("ws://127.0.0.1:24455") as ws:
        await call(ws, "commands.register", name="ping")

        async for raw in ws:
            event = json.loads(raw)
            if event.get("type") == SEND and event.get("command") == "ping":
                who = event["args"][0] if event["args"] else "world"
                await ws.send(json.dumps({"cmd": ADD, "text": f"§apong, {who}"}))

asyncio.run(plugin())
```

`/ping alpha` now prints `pong, alpha` in the chat box. Also `commands.unregister` and
`commands.list`, both optional — a claim is dropped when the connection closes.

### `system.*` — chatwire itself

```python
await call(ws, "system.status")   # version, mapping, port, clients, can_call
await call(ws, "system.stats")    # lines seen, sent, added, commands run/dropped
await call(ws, "system.ping")     # {'pong': True}
await call(ws, "system.detach")   # stops chatwire; the connection then closes
```

## Plugins

A plugin is a program holding a socket open. No plugin format, no manifest, nothing to compile,
nothing to restart. Three steps, and chatwire only does the first two:

**1. You register a name.**

```json
{"cmd":"commands.register","name":"ping"}
```

**2. chatwire pushes the invocation** when the player types it:

```json
{"type":"net.minecraft.client.entity.EntityPlayerSP.sendChatMessage",
 "command":"ping","args":["alpha","beta"],"raw":"/ping alpha beta"}
```

`args` is whitespace-split with empties dropped. `raw` is the line as typed — quoting is absent on
purpose, since every quoting scheme differs and guessing wrong would mangle somebody's argument.

**3. You answer however you like**, with an ordinary command:

```json
{"cmd":"net.minecraft.client.entity.EntityPlayerSP.addChatMessage","text":"pong"}
```

Step 3 is deliberately not chatwire's business. A canned reply built into the register call would
be cheaper and is a language: it grows placeholders, then conditionals, and ends as a small
interpreter nobody asked for.

**A command belongs to the connection that registered it.** Its events go there alone — broadcasting
would let every connected tool act on commands it did not register. Ownership is also why
withdrawal is automatic: when a client disconnects its commands go with it, so a crashed plugin
cannot leave `/ping` swallowed with nobody left to answer. If a claimed command cannot be delivered,
the line goes to the server rather than being eaten; `system.stats` counts those as
`commands_dropped`.

**chatwire's own `sendChatMessage` is intercepted too.** A client asking to say `/ping` is swallowed
exactly as if the player had typed it. Do not name a plugin's output after a registered command.

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

The usual way to automate Minecraft chat is a proxy — prismarine, `node-minecraft-protocol`, a
server plugin. Those speak the **network protocol**. chatwire runs **inside the client**.

The difference that matters: a proxy can only see what was **transmitted**. A large share of what
appears in a Minecraft chat box never crossed the network — mod output, client-side command
replies, warnings the client generated. Those begin and end inside the client, and no proxy can
observe them. chatwire hooks the method that *renders* chat, so it sees both, in the order the
player saw them, colour codes intact.

Use a proxy for headless bots, multi-version support, or anything that must run with no game
window — or on anything that is not Windows. Use chatwire when you care what the player saw, or
when you are working with a client whose behaviour is not visible on the wire.

## Design notes

**Calling the game from a socket thread.** A Java call needs a JavaThread, so the old rule was
"only call Java from inside a hook" and chatwire carried a pump on `Minecraft.runTick`. There is no
pump: vmhook enters Java from any thread, by pure VMStructs where the VM publishes a usable polling
page (Java 8–20) and by minimal JNI everywhere else — including Java 17, which Lunar uses. Commands
are synchronous and report what actually happened.

**What the VM permits is not what Minecraft permits.** `sendChatMessage` is genuinely thread-safe:
it reaches `NetworkManager.sendPacket`, which hands off to the channel's event loop. `addChatMessage`
races cosmetically — it inserts at the front of the lists the client renders from, and the worst
outcome is one line drawn twice for a frame.

**It runs inside someone's game**, so: no exception ever reaches the JVM; a client pays for its own
commands; nothing blocks the game thread; hooks come down before anything unloads, with a wait,
because removing them stops threads *entering* a trampoline but cannot evict one already inside.
`system.detach` stops chatwire but leaves the library mapped — unloading it while a game thread
might be inside produced a DEP violation on Minecraft's own thread, twice.

**Swallowing a chat message needs a reason; letting one through needs none.** The command
interceptor is the only hook that changes what the game does. Every failure in it ends with the
player's line going to the server.

**Layering.** `src/chatwire/sdk.hpp` is the only header that includes vmhook (24k lines — putting it
in more than one TU is more than today's compilers can take). `net.hpp` is the only one that
includes Winsock. `chatwire.hpp` includes neither, so a consumer pays for neither.

## Security

**The server binds `127.0.0.1` only, and that is not configurable.** This socket can send chat as
the player and read everything they see. There is no authentication, and that is only defensible
*because* of the bind address.

Chat text is attacker-controlled — any player can say anything — and it flows into the JSON
chatwire emits, so output is escaped.

## Requirements

- **Windows**, x86-64, on a HotSpot JVM. Configure refuses anything else rather than failing
  later in a wall of missing headers.
- **CMake 3.20+** and a C++23 compiler with a working `<print>`: GCC 14+ (developed against GCC
  15.2 via MSYS2) or MSVC 19.37+.

No package manager. `vmhook` is vendored in `ext/`; the only library linked is `ws2_32`. Python is
needed only for the MCP server.

| Option | |
|---|---|
| `-DCHATWIRE_BUILD_TOOLS=OFF` | just the library; drops the `<print>` requirement |
| `-DCHATWIRE_EMBED_DLL=OFF` | ship the exe and the DLL side by side instead of embedding |

## Status

Chat and runtime commands work end to end, verified by injecting into a real Minecraft.
[CI](.github/workflows/ci.yml) builds and checks that chatwire.exe comes out carrying its library;
everything past that needs a live game and is checked by hand.

## Licence

MIT.
