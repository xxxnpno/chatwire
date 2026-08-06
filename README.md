# chatwire

A live WebSocket API into a running **Minecraft 1.8.9** client. Inject it, connect a WebSocket,
and read and drive the game from any language over a socket.

You can also **add commands to the game while it runs**: claim `/ping` over the socket and chatwire
stops the line before it reaches the server and hands it to you instead. A plugin is a program
holding a socket open — nothing to compile, nothing to restart. See
[Plugins](#plugins-commands-added-while-the-game-is-running).

Feature-based and extensible; chat is the first one, not the shape of the project. C++23, built on
[vmhook](https://github.com/xxxnpno/vmhook): HotSpot introspection, **no JVMTI, no mod loader,
minimal JNI**. Handles all three 1.8.9 mappings (MCP/SRG/OBF). It attaches to a vanilla client the
same way it attaches to a modded one, and to Lunar the same way it attaches to either.

Works on **Minecraft 1.8.9 whatever JVM it runs on** — Java 8 through 26. That distinction matters
more than it sounds: 1.8.9 is the *game* version, and Lunar Client runs it on Java 17.

**Windows, Linux and macOS**, x86-64 and arm64. The library is the same everywhere; what differs
is how it gets into the game, and that difference is real rather than cosmetic — see
[Getting chatwire into the game](#getting-chatwire-into-the-game).

**Chat is the first feature, not the shape of the project.** A command is the fully-qualified Java
member it reaches — `net.minecraft.world.World.playerEntities` — and a feature is one file that
declares which classes it claims and a handler; everything else, routing, the socket, getting onto
the game thread, the lifecycle, already exists. Player list, inventory, world state and the rest
land as new features, not as new plumbing. See [Adding a feature](#adding-a-feature).

```
┌──────────────┐                            ┌────────────────────────────────────────┐
│  your tool   │   ws://127.0.0.1:24455     │  chatwire (injected)                   │
│  any lang    │◄──────────────────────────►│   ├─ feature registry                  │
│  (a plugin,  │  events out, commands in   │   ├─ hooks GuiNewChat.printChatMessage │
│   if it      │                            │   ├─ hooks EntityPlayerSP.sendChat…    │
│   claims a   │                            │   │    └─ and can STOP the line there  │
│   command)   │                            │   └─ calls Java on the asking thread   │
└──────────────┘                            └────────────────────────────────────────┘
```

Your command runs on **your own socket thread**, which calls into Java directly and answers with
what happened. There is no queue and no tick to wait for — see
[Calling the game from a socket thread](#calling-the-game-from-a-socket-thread).

## What it does today

| Direction | Command | What |
|---|---|---|
| **game → you** | — | every line that reaches the chat box, with and without colour codes |
| **game → you** | — | any command **you claimed**, the moment the player types it |
| **you → game** | `net.minecraft.client.entity.EntityPlayerSP.sendChatMessage` | say it to the server, exactly as if typed |
| **you → game** | `net.minecraft.client.entity.EntityPlayerSP.addChatMessage` | show it only to this client, never transmitted |
| **you → game** | `net.minecraft.world.World.playerEntities` | every player the client has loaded, with name and UUID |
| **you → chatwire** | `commands.register` | claim a slash command in the game, at runtime — [see below](#plugins-commands-added-while-the-game-is-running) |

`chat`, `world` and `commands` are features; `system` adds `status`, `stats`, `ping` and `detach`.
Adding another is a new file and one line — `world` was added exactly that way, and nothing in the
server, the dispatcher or the protocol changed to make it work.

## Plugins: commands added while the game is running

You can add a Minecraft command over the socket. No plugin format, no manifest, nothing to compile
against chatwire, and nothing to restart. Three steps, and chatwire only does the first two:

**1. You register a name — just the name.**

```json
{"cmd":"commands.register","name":"ping"}
```

**2. chatwire alerts you when it is typed, with the arguments already split.**

From that moment, `/ping alpha beta` in the game **does not go to the server**. chatwire swallows
the line and pushes it down your socket:

```json
{"type":"net.minecraft.client.entity.EntityPlayerSP.sendChatMessage",
 "command":"ping","args":["alpha","beta"],"raw":"/ping alpha beta"}
```

`args` is whitespace-split with empties dropped, so `/ping   alpha  beta ` is still
`["alpha","beta"]`. `raw` is the line exactly as typed, for anything that wants to apply its own
rules — quoting is deliberately absent, because every quoting scheme differs and guessing wrong
would silently mangle somebody's argument.

**3. You do the logic, and answer however you like.** chatwire takes no view on this; it is an
ordinary command like any other:

```json
{"cmd":"net.minecraft.client.entity.EntityPlayerSP.addChatMessage","text":"pong"}
```

Step 3 is deliberately *not* chatwire's business. It never sees your logic, which is what lets a
plugin be a shell script, a Python daemon or a service on the other side of the machine.

That round trip **is** the plugin system. A plugin is a program holding a socket open — in any
language — and it is added and removed while the game runs. It is about fourteen lines of Python,
[shown in full below](#every-feature-in-python).

### A registered command belongs to the client that registered it

Its events go **there and nowhere else**, and that is not a detail of the implementation — it is
the reason the feature is usable. Broadcasting would have been fewer lines and wrong twice over:
every other connected tool would see commands it did not register, and two plugins claiming `/ping`
would both act on it. So a second client is told `already registered by another client` rather than
quietly sharing.

Ownership is also what makes withdrawal automatic. **When a client disconnects, its commands go with
it.** Without that, a plugin that crashed would leave `/ping` claimed forever: the player types it,
chatwire eats the line, and nothing answers — a game that has quietly stopped working with no error
anywhere. Re-registering something you already own is a no-op, so a plugin that reconnects can
simply replay its whole list.

If a command is claimed but cannot be delivered — the owner vanished between the keystroke and the
write — **the line goes to the server** rather than being eaten. `system.stats` counts those as
`commands_dropped`, because from inside the game that outcome is indistinguishable from a command
that was never registered.

### What it deliberately is not

The cheaper design is a canned reply: register `/ping` *with* the text `pong` and let chatwire
answer by itself, with no round trip. It was rejected because it is a language. The moment somebody
wants an argument in the reply, or a lookup, or a condition, the canned string grows placeholders,
then conditionals, and chatwire ends up owning a small interpreter nobody asked for. Handing the
event to a real program costs one round trip on loopback and can never need a feature. The reference
event to a real program costs one round trip on loopback and can never need a feature.

### The honest costs

- **`commands.register` is the one command with a short prefix that is not `system`.** It reaches
  nothing in the game — it writes a name into a table chatwire owns — so there is no Java member to
  name it after and nothing for a reader to check it against. That is the same exemption `system`
  gets, and the only grounds on which either is allowed.
- **The event shares its name with a command you can send.** `type` is
  `net.minecraft.client.entity.EntityPlayerSP.sendChatMessage` because that is the method it comes
  out of, exactly as the chat event is named after `printChatMessage`. It is not a collision: `type`
  is what happened in the game, `cmd` is what you are asking for, and they are different keys.
- **chatwire's own `sendChatMessage` is intercepted too.** A client asking to say `/ping` gets
  swallowed exactly as if the player had typed it — which is the honest reading of "as if typed",
  and is how one plugin can drive another. It does mean such a call is answered `{"sent":true}` for
  a line that never left. Do not name a plugin's *output* after a registered command.
- **This is the one hook that changes what the game does.** Every other detour observes. It uses
  vmhook's `return_value::cancel()`, which suppresses the method body, so the chat packet is never
  built. If anything at all goes wrong in that path — no callback, no owner, an exception — the
  message goes to the server. The safe direction is always "let it through".
- **Delivery happens on Minecraft's own thread**, inside the detour, as a single socket write. That
  is the same exposure the chat observer already has when it broadcasts from `printChatMessage`: a
  peer that has stopped reading can stall whoever is writing to it. Loopback and a handful of local
  tools is what makes it acceptable, and it is one more reason the socket is not reachable from
  anywhere else.

### Where `args` is decided

The three text rules — what a typed line invokes, what its arguments are, and what a registerable
name looks like — live in [`include/chatwire/command_line.hpp`](include/chatwire/command_line.hpp)
rather than inside the feature. That is the only part of the plugin machinery that means anything
without a JVM, so it is where the [test suite](#tests) can reach it: `/ping   alpha  beta ` giving
`["alpha","beta"]`, the command name never being its own first argument, and a name
`commands.register` accepts being exactly what `invoked_name` produces when the player types it.

That last pair matters more than it sounds. If those two functions disagree, a command registers
successfully and then never fires — a failure with no error anywhere, which is the worst kind.

### A command IS the Java member it reaches

There is one spelling of a command, and it is the fully-qualified Java member:

```
net.minecraft.client.entity.EntityPlayerSP.sendChatMessage
net.minecraft.client.entity.EntityPlayerSP.addChatMessage
net.minecraft.world.World.playerEntities
```

That is not decoration and not a naming style. It names the exact field or method, so you can check
what you are getting against Minecraft's source instead of trusting a summary — including *which*
overload on *which* type. Commands therefore split at the **last** dot, since a class name has dots
of its own. Use the deobfuscated names whatever mapping the target runs; chatwire translates.

**The short prefixes `chat.` and `world.` are gone**, along with the short verbs `send`, `add` and
`players`. They were kept for a while as conveniences and that was the problem: a short name
promises nothing and cannot be checked against anything, so the spelling most people typed was the
one carrying the least information. `world.players` in particular *reads* like the server's roster
and is not — see [the note below](#protocol). If you have a client written against the old
spellings, it needs updating; there is no compatibility path, and a `{"ok":false}` naming the
missing prefix is a better failure than a silent second vocabulary.

`system` is the exception and keeps its short prefix. It reaches nothing in the game — version,
port, counters, detach — so there is no Java member to name and nothing to check against.

## chatwire is not a proxy

The usual way to automate Minecraft chat is a proxy — [prismarine](https://prismarinejs.github.io/)
/ `node-minecraft-protocol`, or a plugin on the server. Those speak the **network protocol**.
chatwire runs **inside the game client**, and the difference is not a matter of taste.

|  | proxy / protocol library | chatwire |
|---|---|---|
| Where it sits | between client and server, or on the server | inside the running client's JVM |
| What it sees | packets on the wire | what the player sees on screen |
| Client-only chat | invisible — it never crossed the network | visible |
| Needs the real game running | no (a proxy works headless) | yes |
| Server can tell | a proxy is a second connection | nothing on the wire changes |
| Versions | many, via protocol definitions | 1.8.9 on HotSpot; Windows, Linux, macOS |
| Language | usually JS/Python | anything that speaks WebSocket |

The line that matters is the third one. A proxy can only ever see what was **transmitted**.
A large share of what appears in a Minecraft chat box was never transmitted at all: mod output,
client-side command replies, `[CHAT]` lines a mod drew itself, warnings the client generated.
Those are `addChatMessage` calls that begin and end inside the client, and no proxy — however
well written — can observe them, because there is nothing to observe.

chatwire hooks the method that **renders** chat, so it sees the rendered truth: everything from
the network *and* everything generated locally, in the order the player actually saw it, with the
colour codes intact.

The trade runs both ways, and honestly:

- **Use a proxy** for headless bots, multi-version support, anything that must run without a
  game window, or anything where you would rather not touch the client's memory.
- **Use chatwire** when you care what the *player* saw, when you need client-side-only messages,
  when you want to inject chat that only the local player sees, or when you are working with a
  client (Lunar, Forge, a private mod) whose behaviour is not visible on the wire.

They also compose: nothing stops a proxy and chatwire running at once, and they answer different
questions.

## All three mappings

Minecraft 1.8.9 ships under three different naming schemes, and chatwire handles all of them.
The same field is:

| Mapping | Class | Field | Seen in |
|---|---|---|---|
| **MCP** | `net/minecraft/client/Minecraft` | `thePlayer` | a deobfuscated dev workspace — ForgeGradle `runClient`, an MCP setup |
| **SRG** | `net/minecraft/client/Minecraft` | `field_71439_g` | **an installed Forge client**, and Searge-mapped builds generally |
| **OBF** | `ave` | `h` | the vanilla jar Mojang ships |

A shipped Forge installation is **SRG**, not MCP: Forge reobfuscates to Searge names for release, so
the client a player actually launches has `field_71439_g` rather than `thePlayer`. MCP names only
survive in a development workspace, where the mod author is running from decompiled sources. That
distinction matters here because both look identical at the class level — see the probe below.

Detection is by **probing the JVM**, never by guessing from a launcher name or a jar hash:

```
class net/minecraft/client/Minecraft exists?
├─ yes ─ field theMinecraft exists?  ──► MCP
│        field field_71432_P exists? ──► SRG
│        neither                     ──► unsupported build, refuse to inject
└─ no ── class ave exists?           ──► OBF
         no                          ──► not Minecraft 1.8.9
```

MCP and SRG share class names and differ only in members, so the class check cannot separate
them — only a field probe can. That is why the order is what it is.

**Per-feature coverage, honestly.** `chat`, `commands` and `system` carry all three mappings —
`commands` hooks `EntityPlayerSP.sendChatMessage`, which `chat` already needed, so it inherited the
table entry rather than adding one. `world` carries MCP and SRG but not OBF — the obfuscated names
for `playerEntities`, `getName` and `getUniqueID` are not in the table, so on a raw vanilla jar that
command fails cleanly rather than guessing. Add them to `mapping.hpp` and it works; a wrong guess
there would call the wrong method, which is worse than an honest failure.

## Quick start

```bash
cmake -S . -B build/etc -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/etc
```

`build/` then holds the executables and nothing else; `build/etc/` holds every file CMake and
Ninja need to produce them. It is that way round because `CMakeCache.txt`, `CMakeFiles/` and
`build.ninja` are not relocatable — they live in the build directory by definition — so the only
way to keep them out of the directory you look in is to make that directory the parent.
Configuring straight into `build` still works and puts it all together, which is what a tool that
calls CMake itself will expect.

You get **one binary**:

| | What it is |
|---|---|
| `build/chatwire.exe` | **Windows** — chatwire itself, and the thing that puts it into the game |
| `build/chatwire-preload` + `chatwire.so`/`.dylib` | **Linux and macOS** — start the game with the library already in it |

On Windows the library is carried **inside** `chatwire.exe` as a resource, so that is the only
file you need: no folder to keep together, and no way to run an injector from one build against a
library from another. It unpacks itself to a temporary directory named after the bytes it
contains, which is what makes a second run reuse the unpacked copy rather than fight the game for
a file it still has mapped. `chatwire.dll` is therefore an intermediate and lives in
`build/etc/lib/` with the object files; `--dll <path>` overrides it, which is what you want when
you are working on the library itself.

The consumer side is Python, in [`python/`](python/), and needs no build step at all — see below.

On Windows, start Minecraft and then run it:

```
> chatwire
chatwire

  library: C:\Users\you\AppData\Local\Temp\chatwire\5d48b3d08080c854\chatwire.dll   (built in)
  target : pid 18244
  port   : 24455   (no console)

  injecting...

  injected.  connect to  ws://127.0.0.1:24455
```

On Linux and macOS, start the game *through* chatwire instead:

```bash
./build/chatwire-preload -- java -jar launcher.jar
```

Either way, you now have a socket. Six lines is a working consumer:

```python
import asyncio, json, websockets                # pip install websockets

async def main():
    async with websockets.connect("ws://127.0.0.1:24455") as ws:
        await ws.send(json.dumps({
            "cmd": "net.minecraft.client.entity.EntityPlayerSP.addChatMessage",
            "text": "§ahello from Python"}))     # only this player sees it
        async for raw in ws:
            print(json.loads(raw))                     # every line in the chat box

asyncio.run(main())
```

There is no chatwire client library, and that is the point: the API is a WebSocket and a flat JSON
object, so a consumer that needed a package to talk to it would be evidence against the design.
[Every feature, in Python](#every-feature-in-python) has a snippet for each command; the only
Python that ships is [`python/mcp_server.py`](python/mcp_server.py), which is a
[program in its own right](#an-ai-in-the-game-the-mcp-server) rather than a library to import.

## Getting chatwire into the game

This is the one place the three platforms genuinely differ, and the difference is not a gap in
the port — it is what each operating system is willing to let one process do to another.

| | Windows | Linux | macOS |
|---|---|---|---|
| Attach to a game **already running** | ✅ `chatwire.exe` | ❌ | ❌ |
| Start the game **with** chatwire | ✅ (inject right after) | ✅ `chatwire-preload` | ✅ `chatwire-preload` |
| Everything past that point | identical | identical | identical |

**Windows** has a supported way to load a library into another process — `CreateRemoteThread` plus
`LoadLibrary` — so chatwire attaches to a game you are already playing.

**Linux** would need `ptrace`: either root, or a machine where `/proc/sys/kernel/yama/ptrace_scope`
has been turned down, plus hand-written per-architecture code to hijack a thread into calling
`dlopen`. It is possible and it is not shipped. That is a debugger attaching to a live game, with
a corrupted process as the failure mode, and the launch-time route costs one word on a command
line.

**macOS** would need `task_for_pid`, which a **hardened-runtime** binary refuses no matter who is
asking — and every JVM people actually play Minecraft on is signed that way. Root does not help.
Only disabling SIP does, and nobody should disable SIP to read chat.

So off Windows chatwire goes in through the dynamic loader:

```bash
chatwire-preload [--port N] [--console] [--verbose] [--timeout SECONDS] -- <command to start the game>

chatwire-preload -- java -jar launcher.jar
chatwire-preload --port 9000 --console -- ./minecraft-launcher
```

which is `LD_PRELOAD` on Linux and `DYLD_INSERT_LIBRARIES` on macOS. Being present *before* the JVM
exists is fine and is not a workaround: `chatwire::start` already polls for Minecraft's classes with
a timeout, because on Windows it had to cope with being injected into a game still sitting on its
launcher screen. The same loop covers "there is no JVM yet". If your launcher takes longer than two
minutes to reach the game, raise `--timeout`.

**One macOS caveat, stated plainly.** `dyld` strips every `DYLD_*` variable when it execs a binary
with the hardened runtime or a restricted entitlement. A JDK you unpacked yourself works; the
`java` inside a signed launcher bundle does not, and the symptom is chatwire never appearing with
no error anywhere. If that happens, point `chatwire-preload` at a plain JDK's `java` instead of at
the launcher.

### After `system.detach`, on POSIX

Detaching leaves the library mapped but stopped — the same on all three platforms, and for the
same reason ([below](#stability)). On Windows running `chatwire.exe` again starts the resident copy,
because it has a named event to set. On Linux and macOS there is nothing to set it *with*: a
second `LD_PRELOAD` is not something that can happen to a running process, and `dlopen`ing the
library again would only bump a reference count without re-running its initialiser. So there,
starting again means restarting the game.

### Every feature, in Python

The API is a WebSocket and a flat JSON object, so this is the whole client library. The snippets
below use [`websockets`](https://pypi.org/project/websockets/) because it keeps them short; any
WebSocket library in any language does the same job, and `python/mcp_server.py` in this repo talks
to chatwire with nothing but the standard library.

```bash
pip install websockets
```

**Connect, and bind the names once.** Commands are long because each one is the Java member it
reaches; write them a single time and the length stops mattering.

```python
import asyncio, json, websockets

PLAYER  = "net.minecraft.client.entity.EntityPlayerSP."
SEND    = PLAYER + "sendChatMessage"                            # to the SERVER
ADD     = PLAYER + "addChatMessage"                             # to me only
PLAYERS = "net.minecraft.world.World.playerEntities"
CHAT    = "net.minecraft.client.gui.GuiNewChat.printChatMessage"

async def call(ws, cmd, **arguments):
    """Sends one command and returns its result."""
    await ws.send(json.dumps({"cmd": cmd, **arguments}))
    reply = json.loads(await ws.recv())
    if not reply["ok"]:
        raise RuntimeError(reply["error"])
    return reply["result"]
```

`call` skips one thing for brevity, and it is worth knowing: chatwire **pushes** events at any
moment, so `recv()` here can return a chat line rather than your answer. A real client checks for
`"ok"` and puts anything else aside — see the reader loop further down.

---

**Say something to the server**, exactly as if the player typed it. Other players see it, and a
leading `/` runs a real command. Refused over 100 characters, which is Minecraft's own limit.

```python
await call(ws, SEND, text="hello everyone")            # {'sent': True}
```

**Say something only to this player.** Nothing is transmitted, no server is involved — this draws
a line in the local chat box and nothing more. `§a` is green.

```python
await call(ws, ADD, text="§athis is client-side only")  # {'added': True}
```

These two are the most important distinction in the API and the easiest to confuse, which is why
they are separate commands rather than one with a flag.

---

**The players the client has loaded**, each with a name and a UUID from the same object in one
pass, so an entry's two halves always belong together.

```python
result = await call(ws, PLAYERS)
for who in result["players"]:
    print(who["name"], who["uuid"])
print(result["count"], "loaded by this client")
```

Not the server's roster — these are the players near enough to exist as entities, which on a large
server is a small fraction of the tab list. The command is named after the field it reads so the
answer cannot be mistaken for something else.

---

**Every line that reaches the chat box**, as it happens. Server messages, mod output, client-side
replies, death messages — one hook catches all of it, so this sees what the *player* saw.

```python
async for raw in ws:
    event = json.loads(raw)
    if event.get("type") == CHAT:
        print(event["plain"])          # "formatted" keeps the § colour codes
```

---

**Add a command to the game.** Claim the name, and from that moment typing it does not reach the
server: chatwire swallows the line and hands it to you with the arguments already split. This is
the whole plugin system, and it is fourteen lines.

```python
async def plugin():
    async with websockets.connect("ws://127.0.0.1:24455") as ws:
        await call(ws, "commands.register", name="ping")

        async for raw in ws:
            event = json.loads(raw)
            if event.get("type") == SEND and event.get("command") == "ping":
                # The player typed it; the server never saw it.  Answering is
                # entirely our business -- chatwire takes no view.
                who = event["args"][0] if event["args"] else "world"
                await ws.send(json.dumps({"cmd": ADD, "text": f"§apong, {who}"}))

asyncio.run(plugin())
```

`/ping alpha` in the game now prints `pong, alpha` in the player's chat box.

`event["type"]` is the method the event came **out of** — `EntityPlayerSP.sendChatMessage` — which
is also the name of a command you can send. Not a clash: `type` is what happened in the game, `cmd`
is what you are asking for, and they are different keys.

Give the name back, or see who owns what:

```python
await call(ws, "commands.unregister", name="ping")
await call(ws, "commands.list")     # {'count': 1, 'commands': [{'name': ..., 'client': 3}]}
```

Both are optional. A claim is dropped automatically when the connection closes, so a script that
crashes cannot leave `/ping` swallowed with nobody left to answer it.

---

**Ask chatwire about itself**, and stop it:

```python
await call(ws, "system.status")   # version, mapping, port, clients, can_call
await call(ws, "system.stats")    # lines seen, sent, added, commands run/dropped
await call(ws, "system.ping")     # {'pong': True}
await call(ws, "system.detach")   # stops chatwire; the connection then closes
```

---

**Putting it together** — the shape a real client has, where one reader owns the socket and
separates replies from pushed events:

```python
async def main():
    async with websockets.connect("ws://127.0.0.1:24455") as ws:
        await ws.send(json.dumps({"cmd": "commands.register", "name": "ping"}))

        async for raw in ws:
            message = json.loads(raw)

            if "ok" in message:                      # a reply to something we sent
                print("reply:", message)
            elif message["type"] == CHAT:            # a line in the chat box
                print(message["plain"])
            elif message["type"] == SEND:            # a command we claimed
                await ws.send(json.dumps({"cmd": ADD, "text": "§apong"}))

asyncio.run(main())
```

That is every feature chatwire has today.

### An AI in the game: the MCP server

[`python/mcp_server.py`](python/mcp_server.py) exposes a running Minecraft client over the
**Model Context Protocol**, so an assistant can read the chat, say things, list the players, and
own a slash command:

```bash
pip install mcp
claude mcp add chatwire -- python /path/to/python/mcp_server.py
```

| Tool | What it does |
|---|---|
| `read_chat` | recent lines from the chat box — what the **player** saw, mod output and all |
| `list_players` | the players the client has loaded, with UUIDs |
| `game_status` | chatwire's version, mapping and port |
| `say` | **public.** Every player on the server sees it, as the player |
| `tell` | client-side only. Nobody else sees it |
| `claim_command` / `release_command` | take `/name` in the game, or give it back |
| `take_commands` | invocations of your claimed commands, since the last call |

Two things about that list are deliberate.

**`say` and `tell` are separate tools, not one with a flag.** This reaches a real game that a real
person is playing, and public-versus-private is the easiest mistake to make and the most
embarrassing one to make on somebody else's behalf. A model choosing between two differently-named
tools cannot make it by omission.

**`read_chat` is a view; `take_commands` is a queue.** Reading chat twice shows the same lines, and
missing some while the model was thinking about something else is fine. A command invocation is
*work* — the player typed it and is waiting — so it is handed over exactly once, and two calls
cannot both act on it and answer twice.

The server connects lazily and reconnects on demand, because an MCP client is usually started
before the game is, and one that died at launch because Minecraft was not up yet would be useless
in exactly the normal case.

The port is `24455` by default, or set `CHATWIRE_PORT` in the game's environment.

## Protocol

Every message is one flat JSON object. A command is `<class>.<member>`, split at the **last** dot.

**Pushed to you, unprompted:**

```json
{"type":"net.minecraft.client.gui.GuiNewChat.printChatMessage",
 "formatted":"§b[Team] §fhi","plain":"[Team] hi"}
```

`formatted` keeps the `§` colour codes; `plain` has them stripped. Use whichever suits.

And, if you have claimed a command with `commands.register`, every time the player types it:

```json
{"type":"net.minecraft.client.entity.EntityPlayerSP.sendChatMessage",
 "command":"ping","args":["a","b"],"raw":"/ping a b"}
```

`args` is whitespace-split with empties dropped, which is what every Minecraft command parser does;
`raw` is the line exactly as typed, for anything that wants to parse it its own way. This one goes
**only** to the client that registered that command — see
[Plugins](#plugins-commands-added-while-the-game-is-running).

**Sent by you:**

| Command | Arguments | Effect |
|---|---|---|
| `net.minecraft.client.entity.EntityPlayerSP.sendChatMessage` | `text` (≤100 chars) | Sends to the server. A leading `/` runs a command. Reaches `sendChatMessage(String)`. |
| `net.minecraft.client.entity.EntityPlayerSP.addChatMessage` | `text` | Client-side only. Nobody else sees it. Reaches `addChatMessage(IChatComponent)`. |
| `net.minecraft.world.World.playerEntities` | — | Every player the client has loaded, each with `name` and `uuid`. Reads the field, plus `Entity.getName()` / `getUniqueID()`. |
| `commands.register` | `name` | Claims `/name` in the game for **this** connection. A leading `/` is allowed and ignored. |
| `commands.unregister` | `name` | Gives it back. Only the owner may. |
| `commands.list` | — | Every claimed command, with the `client` that owns it. |
| `system.status` | — | Version, mapping, bound port, connected clients, and `can_call` (whether this JVM lets chatwire reach the game). |
| `system.stats` | — | Counters: lines seen, messages sent, messages added, commands run, commands dropped. |
| `system.ping` | — | Liveness. |
| `system.detach` | — | **Unloads chatwire from the game.** |

Every command is the member it reaches, and the pushed event is the method it comes out of. If you
have read Minecraft's source, you already know what each one does — and, more usefully, you know
the difference between the two chat ones.

Three names moved when the short spellings went, and old clients will notice:

`commands` is the second feature with a short prefix, and it earns that the same way `system` does:
`commands.register` reaches nothing in the game — it writes a name into a table chatwire owns — so
there is no Java member to name it after and nothing for a reader to check it against. Anything that
*touches* Minecraft still spells out the member it reaches, which is why the command's own events
are named `net.minecraft.client.entity.EntityPlayerSP.sendChatMessage`.

| Was | Is now |
|---|---|
| `chat.send`, `chat.sendChatMessage` | `net.minecraft.client.entity.EntityPlayerSP.sendChatMessage` |
| `chat.add`, `chat.addChatMessage` | `net.minecraft.client.entity.EntityPlayerSP.addChatMessage` |
| `world.players`, `world.playerEntities` | `net.minecraft.world.World.playerEntities` |
| `chat.stats` | `system.stats` |
| `type: "chat"`, `type: "printChatMessage"` | `type: "net.minecraft.client.gui.GuiNewChat.printChatMessage"` |

`chat.stats` moved rather than being renamed: there is no `stats` method in Minecraft, so naming
one after a game class would be the single thing these names promise never to do. The counters are
chatwire's own bookkeeping, and that is what `system` is for.

`system.detach` replies *before* it acts, so you get the acknowledgement and then the
connection closes — that is the detach working, not a failure.

**It stops chatwire but leaves the library mapped**, and that is deliberate. The argument is not
Windows-specific and neither is the conclusion — `dlclose` unmaps exactly the way `FreeLibrary`
does, and a thread inside a trampoline dies exactly the same way — so this is the behaviour on all
three platforms. Removing a hook stops
threads *entering* it; it cannot evict one already inside, and vmhook keeps no in-flight count —
so there is no instant at which unloading is provably safe while the game runs. Unloading anyway
produced a DEP violation on Minecraft's own thread, twice. Detaching costs about a megabyte of
address space until the game exits, and in exchange it cannot kill the process it is detaching
from. Everything observable is gone either way: socket closed, hooks out. Running `chatwire.exe`
again starts the resident copy.

It cannot run on the requesting client's own thread: shutdown joins every client thread, so a
detach handled inline would join itself and deadlock. It is handed to a thread of its own, which
waits long enough for the reply to be written, stops chatwire, and unloads.

That single thread does all of it deliberately. Unloading ends in `FreeLibraryAndExitThread`,
which makes the unload safe for **the thread that calls it and no other** — any other thread
still executing code in the library when it runs is left on freed pages and dies at some
unpredictable later moment. So the detach path spawns exactly one thread and that thread is the
one that unloads; nothing is left behind mid-call.

`net.minecraft.world.World.playerEntities` answers with the players the **client** has loaded —
those near enough to exist as entities. It is not the server's roster, and on a large server it is
a small fraction of the tab list. That is a property of Minecraft, and the command is the field it
reads so the answer cannot be mistaken for something else — which is exactly why the short
`world.players` was withdrawn rather than kept: it read like the roster:

```json
{"count":2,"players":[{"name":"Steve","uuid":"8667ba71-..."},{"name":"Alex","uuid":"ec561538-..."}]}
```

Name and UUID come from the same object in one pass, so an entry's two halves always belong
together.

**Every command gets a reply:**

```json
{"ok":true,"result":{"sent":true}}
{"ok":false,"error":"'text' exceeds the 100-character chat limit"}
```

`sendChatMessage` and `addChatMessage` are **synchronous**: `sent` / `added` means the
call into Minecraft has already happened, not that it was accepted for later. If you are not in
a world, or the call failed, you get `{"ok":false,"error":"not in a world"}` instead of a
success you would have had to disbelieve.

Until 0.3.0 these answered `{"queued":true}` and ran on the next client tick. That reply is
gone, along with the queue and the tick — see below.

### sendChatMessage vs addChatMessage

This is the most important distinction in the API and the easiest to get wrong, which is why
they are separate verbs rather than a flag:

- **`sendChatMessage`** goes to the server. Other players see it. `/` commands execute. You are
  talking.
- **`addChatMessage`** only draws in your own chat box. Nothing is transmitted. You are annotating.

## Design

### Calling the game from a socket thread

A Java call needs a **JavaThread** — the VM-side object holding the frame anchor a GC stack-walk
follows and the state a safepoint reads. A native thread HotSpot has never seen has none of that,
and calling on one corrupts the heap the first time a collection runs mid-call. That is why the
old rule was "only call Java from inside a hook", and why chatwire used to carry a pump: a detour
on `Minecraft.runTick` that queued work onto the game thread.

There is no pump any more. `sdk::send_chat` and `sdk::add_chat` are plain synchronous functions a
WebSocket thread calls directly and gets a real `true`/`false` back from. vmhook makes that safe,
by one of two routes it picks from what the JVM publishes:

| Route | When | How |
|---|---|---|
| pure VMStructs | the VM publishes a usable `os::_polling_page` — Java 8–20 | claim a state the VM must wait for, verify no safepoint has begun, then enter Java |
| minimal JNI | everything else, including **Java 17, which Lunar uses** | JNI's own entry performs the real safepoint-checked transition, and its references are GC-tracked |

**On the JNI part, precisely.** chatwire itself contains no JNI. vmhook uses exactly two JNI
functions on the off-hook path — `NewStringUTF` to build a String argument and `Call<T>MethodA` to
make the call — plus a few id lookups, reached by index into the function table rather than by
including `jni.h`. Inside a hook none of it is used: there the thread is already `_thread_in_Java`
and the pure call stub applies. The reason it cannot be avoided on a modern JVM is documented at
length in vmhook's README, with the measurements behind it; the short version is that the one bit
required — *has a safepoint begun?* — is published nowhere on JDK 21+, and a thread the VM has
already counted as safe cannot be un-counted.

#### What the VM permits is not what Minecraft permits

Being legal for the VM is not the same as being safe for Minecraft, and the honest answer is
per-method rather than global:

- **`sendChatMessage` is genuinely thread-safe.** It reaches `NetworkManager.sendPacket`, which
  is one of the few parts of the 1.8.9 client written to be called from anywhere: it guards its
  outbound queue with a `ReentrantReadWriteLock` and, when the caller is not the channel's event
  loop, hands the write to that event loop instead of doing it inline. The game's own network
  threads depend on this.
- **`addChatMessage` races, cosmetically.** It ends in `GuiNewChat.setChatLine`, which inserts
  at the front of the two `ArrayList`s the client thread renders from. `ArrayList.add(0, e)`
  populates the new array, shifts, stores, and increments `size` *last*, so a reader indexing
  below `size` never sees a null or an out-of-range index — the worst it observes is one line
  drawn twice for a single frame. The 100-line trim removes from the tail; `drawChat` reads from
  the head and stops around twenty lines in, so the two never meet. It is a data race whose
  failure mode is a flicker, at one call per client request, and it is not worth a detour in the
  client's main loop.

If it ever has to be exact, the way to do it without a hook is to hand Minecraft an
`S02PacketChat` and let `PacketThreadUtil` schedule it — the client already marshals its own
inbound chat that way.

Two VM-level hazards used to sit here as well — an oop held across a call that the callee's own
allocation could move, and vmhook bump-allocating out of another thread's TLAB. **Both are closed
on the JNI path**, and closing them is most of why it is worth having: a JNI reference is one the
collector tracks and updates, so an object cannot move out from under a call, and objects are
allocated by the VM rather than by writing into a lockless bump pointer somebody else owns.

The reads are unaffected by any of this: a field get is a load from an address and has always
worked from any thread. Only calling ever needed permission.

This is also what keeps the next feature cheap. A player list, an inventory read or a world
query has the same shape as a chat send — attach, call, answer — so a new feature inherits the
threading model instead of having to re-decide it.

### Stability

It runs inside someone's game. The rules that follow from that:

- **No exception ever reaches the JVM.** Every detour body is `noexcept` and catch-all
  guarded. An exception unwinding into Minecraft's interpreter frame has no handler.
- **A client's own thread pays for its own commands.** A client spamming chat blocks itself,
  not the game and not the other clients. This replaced a bounded queue that dropped the oldest
  task under load — backpressure that lands on the peer causing it is better than backpressure
  that loses someone else's message.
- **Nothing blocks the game thread.** A client that stops reading gets dropped, not waited on.
- **Swallowing a message needs a reason; letting one through needs none.** The command interceptor
  is the only hook that changes what the game does, and every failure in it — no owner, no delivery
  route, an exception, chatwire shutting down — ends with the player's line going to the server. A
  bridge that silently ate someone's chat would be worse than one that was never injected.
- **No lock is held across a call into Java.** The command table is shared between the game thread
  (inside the detour) and the socket threads (registering), so it has a mutex — and a socket thread
  that entered Java while holding it could be stopped at a safepoint the game thread is waiting to
  reach, with the game thread blocked behind that same lock. Everything copies what it needs out
  from under the lock and acts after releasing it.
- **Threads let go of the VM.** An attached thread is released when it exits, and explicitly
  before the library would unload — the VM must not be left holding a JavaThread for a thread that is
  about to vanish, whose stack a later safepoint would try to walk.
- **Unhooking waits before unloading.** `remove_hooks()` stops threads *entering* a trampoline;
  it cannot evict one already inside, and vmhook keeps no in-flight count. The thread that enters
  the `printChatMessage` detour is Minecraft's own and is never joined, so there is no moment at
  which "nobody is inside" can be proven. Teardown unpatches, waits several ticks, and only then
  lets the library go. Skipping that wait killed a game during testing.
- **Threads are joined before anything unloads.** `stop()` closes the listener first (which
  wakes `accept`), shuts down client sockets (which wakes their readers), *then* joins.
- **Objects that own JVM state are never destroyed implicitly.** Hook handles and the server
  are leaked on purpose: their destructors join threads and remove detours, and running that
  during library unload — under the loader lock — is a deadlock. `chatwire_stop()` is the explicit,
  correctly-ordered path.
- **`DllMain` does nothing but spawn a thread.** Everything else would deadlock the loader lock.

### Layering

```
src/dllmain.cpp         Win32 loader entry; spawns a thread and returns
src/soload.cpp          the same job for Linux and macOS: an ELF/Mach-O initialiser
src/entry.hpp           what those two both do once they are off the loader lock
src/chatwire.cpp        start / stop -- the only TU that sees vmhook AND sockets
include/chatwire/
  chatwire.hpp          the public surface; includes neither heavyweight header
  feature.hpp           the extension point, and the registry
  features/*.hpp        behaviour; knows nothing about vmhook
  features/commands.hpp runtime-registered commands; owns no socket, only a table
  console.hpp           chatwire's own console window, and its commands
  ansi.hpp   config.hpp colour codes; port and options from the environment
  json.hpp   log.hpp    helpers
  command_line.hpp      what a typed line invokes and what its args are; pure
                        text, and the only part of the plugin machinery that
                        can be tested without a JVM
  ws/*.hpp              RFC 6455 by hand; no platform header of its own
  net.hpp               ◄── THE ONLY HEADER THAT KNOWS SOCKETS DIFFER
  module.hpp            ◄── and the only one that knows how to find itself
  terminal.hpp          ANSI and UTF-8, for the standalone tools
tools/injector.cpp      chatwire.exe: finds the game, carries the DLL, injects it
tools/chatwire-preload  the POSIX way in -- LD_PRELOAD / DYLD_INSERT_LIBRARIES
python/mcp_server.py    the protocol, exposed to an AI over MCP.  The only
                        consumer that ships -- everything else is an example
                        in this README
  sdk.hpp               ◄── THE ONLY HEADER THAT INCLUDES vmhook.hpp
  mapping.hpp           the three name tables; pure data, no vmhook
```

`sdk.hpp` is a facade exposing only `std::string`, `bool` and function pointers, so no vmhook
type crosses it. A feature is written in terms of "send this chat message", never in terms of
klasses, oops and detours — which is also why a feature cannot accidentally make the mistake
described below.

### Resolving methods on the object's real class

Worth stating because getting it wrong crashed Minecraft.

`printChatMessage`'s parameter is declared as `IChatComponent`, so the wrapper is registered as
that. But `IChatComponent` is an **interface**: `getFormattedText` on it is an abstract
declaration with no body, and its interpreted entry is HotSpot's abstract-method-error stub.
Resolving the method from the *registered* class found that stub, and invoking it through a
synthetic call frame took the VM down.

The object at runtime is never an `IChatComponent` — it is a `ChatComponentText`, a
`ChatComponentTranslation`, or another concrete subclass, all of which inherit a real
implementation from `ChatComponentStyle`. chatwire therefore asks the oop what it *actually* is
(`vmhook::klass_from_oop`) and walks that hierarchy, **skipping abstract methods**, which is
what virtual dispatch would have picked.

## Adding a feature

Two files, two lines. Write the header:

```cpp
// include/chatwire/features/inventory.hpp
#include "chatwire/feature.hpp"

class inventory_feature final : public chatwire::feature
{
    // What it is called in the log.  NOT a command prefix.
    auto name() const noexcept -> std::string_view override { return "inventory"; }

    // What it answers to: the classes it calls, so a command is the member.
    auto claims(std::string_view prefix) const noexcept -> bool override
    {
        return prefix == "net.minecraft.entity.player.InventoryPlayer";
    }

    auto start() noexcept -> bool override { /* install hooks */ return true; }
    auto stop()  noexcept -> void override { }
    auto handle(const chatwire::command& cmd) noexcept -> chatwire::response override
    {
        if (cmd.verb == "mainInventory")
        {
            return chatwire::response::success(R"({"items":[]})");
        }
        return chatwire::response::failure("unknown member");
    }
};

namespace chatwire::features::inventory
{
    inline auto instance() noexcept -> chatwire::feature*
    {
        static inventory_feature f{};
        return &f;
    }
}
```

Then in `src/chatwire.cpp`:

```cpp
#include "chatwire/features/inventory.hpp"               // 1
registry::add(features::inventory::instance());          // 2
```

`net.minecraft.entity.player.InventoryPlayer.mainInventory` now routes to it. Nothing else
changes — not the server, not the dispatcher, not the protocol.

Overriding `claims()` is what makes a command checkable, so do it for anything that touches the
game: claim the class you call, one `claims()` line per class, and let the verb be the member's own
name. A feature that reaches nothing in Minecraft — `system` is the only one so far — can leave
`claims()` alone and answer to its bare name, because there is no source for a reader to check it
against anyway.

`handle()` runs on the asking client's own socket thread and may call Java straight from it —
the sdk attaches the thread to the VM first. There is nothing to marshal onto and no queue to
feed. What that does *not* give you is a thread-safe Minecraft: check whether the method you are
about to call tolerates being called off the client thread, the way `sendChatMessage` and
`addChatMessage` are each argued about above.

Registration is explicit rather than self-registering via a static initialiser. Static
initialisation order across translation units is unspecified, and a registry populated that way
is a classic "works until you add the fourth feature" bug.

## Security

**The server binds `127.0.0.1` only, and that is not configurable.** This socket can send chat
as the player and read everything they see. Exposing it to the network would hand that to
anyone who can reach the machine. There is no authentication, and that is only defensible
*because* of the bind address.

Chat text is attacker-controlled — any player on the server can say anything — and it flows
into the JSON chatwire emits. Output is escaped so a quote or a newline in a chat line cannot
forge a second JSON field.

## Requirements

- **Windows, Linux or macOS**, x86-64 or arm64, on a HotSpot JVM.
- **CMake 3.20+** and a C++23 compiler with a working `<print>`. That last clause is the one that
  actually bites, so configure checks it and tells you rather than failing later in a wall of
  template output:

| | Needs |
|---|---|
| Windows | GCC 14+ (developed against GCC 15.2 via MSYS2); MSVC 19.37+ |
| Linux | GCC 14+ (`g++-13` is **not** enough — no `<print>`) or Clang 18+ |
| macOS | Homebrew LLVM: `brew install llvm`, then `-DCMAKE_CXX_COMPILER=$(brew --prefix llvm)/bin/clang++`. The system Apple Clang ships a libc++ without `<print>`. |

No modules, no package manager. `vmhook` is vendored in `ext/`; the only libraries linked are the
platform's own — `ws2_32` on Windows, `pthread` and `dl` elsewhere.

**Nothing here needs Python.** It is only for the MCP server, which needs `pip install mcp` and
nothing else; a consumer of your own needs whatever WebSocket library your language already has.

Two options worth knowing:

| | |
|---|---|
| `-DCHATWIRE_BUILD_TOOLS=OFF` | build just the library, which drops the `<print>` requirement entirely |
| `-DCHATWIRE_EMBED_DLL=OFF` | do not carry the library inside `chatwire.exe`; the two ship side by side instead, which is the older layout and is convenient while working on the library |

### What is verified where

Being honest about this, because "cross-platform" is easy to claim and easy to get wrong:

| | Builds & server tests | MCP server | Injected into a real Minecraft |
|---|---|---|---|
| Windows | ✅ CI + locally | ✅ CI + locally | ✅ by hand |
| Linux | ✅ CI, and the library is loaded via `LD_PRELOAD` to prove it resolves | ✅ CI | ❌ **not yet** |
| macOS | ✅ CI, same | ✅ CI | ❌ **not yet** |

The Minecraft half — mappings, hooks, every Java call — needs a running game and is verified by
injecting into one. That has been done on Windows. The POSIX loader path, the sockets and the
protocol are covered by [CI](.github/workflows/ci.yml) on all three; what has *not* been done is
attaching to a real Minecraft on Linux or macOS and watching chat arrive. If you try it, the
mapping detection is the part most likely to surprise us.

**One more line on the plugin feature specifically.** The pieces either side of the hook are
covered: the argument splitting, the connection identity events are routed on, `send_to` refusing
a client that has left, and the MCP server handing an invocation over exactly once. What no test
can cover
is the one thing only a game can — that the detour on `EntityPlayerSP.sendChatMessage` fires, and
that `cancel()` really stops the packet. That step is verified by injecting, the same way the rest
of the Minecraft half is.

**And one about what is no longer checked.** There used to be a `chatwire-mock` that served the
protocol with synthetic chat, and a C++ terminal client, and a CI step that ran the two against
each other end to end.
Both are gone: shipping a program whose job is to pretend to be Minecraft is a second
implementation of the protocol to keep in step, and it was starting to be one. What replaced it is
narrower and honest about being narrower — the C++ suite covers the server and the parsing, and
`tests/test_python.py` covers the clients against a stub that lives in the test file. Nothing now
exercises the real server and a real client over a real socket in one process.

### A note on modules

chatwire was first written as C++26 named modules. That is gone, and the reason is worth
recording: GCC 15 segfaults instantiating vmhook inside a module, neither GCC 15 nor Clang 19
can compile a translation unit that both `import`s a module and includes `<windows.h>`, Clang
crashes outright on a 24k-line header as a module interface, and GCC does not COMDAT
function-local statics of module-attached inline functions. None of that was a defect in this
code, and all of it was time spent on the build rather than on the library.

The layering those constraints forced was worth keeping — one boundary for vmhook, one for the
platform — and it survives as ordinary include discipline. It is also what made the Linux and
macOS port small: `sdk.hpp` is still the only header that includes vmhook; the sockets moved
behind `net.hpp` and `ws/server.hpp` now includes no platform header at all; and
`chatwire.hpp` includes neither, so a consumer's build pays for neither.

## Tests

```bash
cmake --build build && ./build/chatwire_test_server
```

Covers the halves that run without Minecraft: the RFC 6455 handshake (against the worked
example in the RFC), frame length boundaries at 125/126/65535/65536, rejection of unmasked and
reserved-bit frames, oversized control frames, JSON parsing of hostile input, client reaping,
protocol-violation dropping, clean shutdown, and restart on the same port — plus the connection
identity the plugin feature routes on: that the handler is told which client asked, that `send_to`
reaches that one client, and that it refuses both an id nobody holds and an id whose client has
since left. That last one is what stops a command registered by a departed plugin from being
delivered to whoever connects next.

It also covers what a plugin is actually handed: that `/ping alpha beta` invokes `ping` with
`["alpha","beta"]`, that runs of spaces and a trailing one do not become empty arguments, that the
command name is never its own first argument, that `and/or` and a bare `/` are not commands, and
that a name `commands.register` accepts is exactly the one `invoked_name` produces when the player
types it — asserted as a pair, because a command that registers successfully and then never fires
is the failure those two functions can produce by disagreeing.

`tests/test_python.py` covers the other side — both Python programs, against a stub server that
lives in the test file. It is the framing that matters there: the same 125/126/65535/65536
boundaries, in both directions, plus that a client frame is masked (the real server drops one that
is not), that a refusal leaves the connection usable, and that the MCP server tells a pushed event
apart from a reply rather than handing a chat line back as a tool result.

[CI](.github/workflows/ci.yml) runs both suites on Windows, Linux and macOS, and on POSIX loads the
library with `LD_PRELOAD` / `DYLD_INSERT_LIBRARIES` — the cheapest way to catch the failure a
statically-linked Windows build would never reveal, an unresolved symbol in the shared object.

The Minecraft side needs a live game and is verified by injecting.  See
[What is verified where](#what-is-verified-where).

## Status

Chat works end to end and the server half is tested on all three platforms. Commands registered at
runtime work the same way, with the one caveat in
[What is verified where](#what-is-verified-where). The game side is verified by injection rather
than automatically — there is no test harness that runs a Minecraft client — and that injection has
been done on **Windows**. Linux and macOS build, serve the protocol and load into a host process in
CI, but have not yet been pointed at a real game.

## Licence

MIT.
