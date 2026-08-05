# chatwire

A live WebSocket API for **Minecraft 1.8.9**. Inject it, connect a WebSocket, and you can read
every chat line as it appears and send chat back — from any language, over a socket.

Built on [vmhook](https://github.com/xxxnpno/vmhook): pure HotSpot introspection, **no JNI, no
JVMTI, no Forge, no mod loader**. It attaches to a vanilla client the same way it attaches to a
modded one.

```
┌──────────────┐   ws://127.0.0.1:24455    ┌───────────────────────────────┐
│  your tool   │ ◄─────────────────────────►│  chatwire (injected)          │
│  any lang    │   chat lines out           │   ├─ hooks GuiNewChat         │
│              │   commands in              │   ├─ pumps on Minecraft.runTick│
└──────────────┘                            │   └─ speaks to the game       │
                                            └───────────────────────────────┘
```

## What it does today

| Direction | What |
|---|---|
| **game → you** | every line that reaches the chat box, with and without colour codes |
| **you → game** | `chat.send` — say it to the server, exactly as if typed |
| **you → game** | `chat.add` — show it only to this client, never transmitted |

Chat is the first feature, not the only one. The architecture is built around adding more —
see [Adding a feature](#adding-a-feature).

## All three mappings

Minecraft 1.8.9 ships under three different naming schemes, and chatwire handles all of them.
The same field is:

| Mapping | Class | Field | Seen in |
|---|---|---|---|
| **MCP** | `net/minecraft/client/Minecraft` | `thePlayer` | Forge / MCP dev environments |
| **SRG** | `net/minecraft/client/Minecraft` | `field_71439_g` | Searge-mapped builds |
| **OBF** | `ave` | `h` | the vanilla jar Mojang ships |

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

## Quick start

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

You get four binaries:

| Binary | What it is |
|---|---|
| `chatwire.dll` | the library, injected into Minecraft |
| `chatwire-inject.exe` | finds Minecraft and injects the DLL |
| `chatwire-client.exe` | a terminal chat client — the reference consumer |
| `chatwire-mock.exe` | serves the protocol with **fake** chat, so you can build against it with no game running |

Start Minecraft, then:

```
> chatwire-inject
chatwire-inject

  dll   : C:\...\chatwire.dll
  target: pid 18244

  injecting...

  injected.  connect to  ws://127.0.0.1:24455

> chatwire-client
connected.  type to chat, /add for client-side, /quit to exit

[Team] Steve: hello there              ← lines from the game, in colour
Alex joined the game
hey everyone                           ← you typed this; it went to the server
/add just for me                       ← only you see this
```

### Building a consumer without Minecraft

`chatwire-mock` speaks the identical protocol and emits synthetic chat lines. It links the
real server and the real JSON, so the wire format cannot drift from production:

```bash
chatwire-mock --port 24455 --interval 1000
```

Point your tool at it and develop with no game open. The only thing it cannot catch is a
mapping problem inside Minecraft itself.

### From code

Inject `chatwire.dll` with `chatwire-inject` (or any injector). Then:

```js
const ws = new WebSocket("ws://127.0.0.1:24455");

ws.onmessage = (e) => {
    const msg = JSON.parse(e.data);
    if (msg.type === "chat") console.log(msg.plain);   // every line in chat
};

// say it to the server, as if typed
ws.send(JSON.stringify({ cmd: "chat.send", text: "hello world" }));

// show it only to me
ws.send(JSON.stringify({ cmd: "chat.add", text: "§athis is client-side only" }));
```

The port is `24455` by default, or set `CHATWIRE_PORT` in the game's environment.

## Protocol

Every message is one flat JSON object. Commands are `feature.verb`.

**Pushed to you, unprompted:**

```json
{"type":"chat","formatted":"§b[Team] §fhi","plain":"[Team] hi"}
```

`formatted` keeps the `§` colour codes; `plain` has them stripped. Use whichever suits.

**Sent by you:**

| Command | Arguments | Effect |
|---|---|---|
| `chat.send` | `text` (≤100 chars) | Sends to the server. A leading `/` runs a command. |
| `chat.add` | `text` | Client-side only. Nobody else sees it. |
| `chat.stats` | — | Counters: lines seen, messages sent, messages added. |
| `system.status` | — | Mapping, bound port, and pump counters. |
| `system.ping` | — | Liveness. |
| `system.detach` | — | **Unloads chatwire from the game.** |

`system.detach` replies *before* it acts, so you get the acknowledgement and then the
connection closes — that is the detach working, not a failure.

It cannot run on the requesting client's own thread: shutdown joins every client thread, so a
detach handled inline would join itself and deadlock. It is handed to a separate thread that
pauses long enough for the reply to be written, then unloads.

**Every command gets a reply:**

```json
{"ok":true,"result":{"queued":true}}
{"ok":false,"error":"'text' exceeds the 100-character chat limit"}
```

`chat.send` and `chat.add` are **asynchronous**: `queued: true` means the message reached the
game thread's queue, not that Minecraft has processed it. It runs on the next client tick.

### send vs add

This is the most important distinction in the API and the easiest to get wrong, which is why
they are separate verbs rather than a flag:

- **`chat.send`** goes to the server. Other players see it. `/` commands execute. You are
  talking.
- **`chat.add`** only draws in your own chat box. Nothing is transmitted. You are annotating.

## Design

### Everything Java happens on the game thread

vmhook's contract: you may only call into Java from a real JavaThread *inside an interpreter
detour*. Calling from a plain native thread crashes the VM — a GC stack-walk faults on the
missing frame anchor.

chatwire's WebSocket threads therefore **never touch Java**. They hand a task to a pump, and
the pump runs it from inside a detour on Minecraft's own thread:

```
websocket thread              minecraft client thread
────────────────              ───────────────────────
submit(task)     ────────►    Minecraft.runTick() fires
(returns at once)             detour drains the queue and runs the task
```

`Minecraft.runTick` is the natural pump: called every client tick, on the thread that owns the
world. Latency is a fraction of a tick.

### Stability

It runs inside someone's game. The rules that follow from that:

- **No exception ever reaches the JVM.** Every detour body is `noexcept` and catch-all
  guarded. An exception unwinding into Minecraft's interpreter frame has no handler.
- **Bounded queues.** A client spamming faster than the game ticks drops the *oldest* task and
  counts it. Dropping is a bounded, visible failure; growing is an out-of-memory in a game.
- **Nothing blocks the game thread.** A client that stops reading gets dropped, not waited on.
- **Threads are joined before anything unloads.** `stop()` closes the listener first (which
  wakes `accept`), shuts down client sockets (which wakes their readers), *then* joins.
- **Objects that own JVM state are never destroyed implicitly.** Hook handles and the server
  are leaked on purpose: their destructors join threads and remove detours, and running that
  during DLL unload — under the loader lock — is a deadlock. `chatwire_stop()` is the explicit,
  correctly-ordered path.
- **`DllMain` does nothing but spawn a thread.** Everything else would deadlock the loader lock.

### Layering

```
dllmain.cpp             Win32 entry; spawns a thread and returns
src/chatwire.cpp        start / stop -- the only TU that sees vmhook AND Winsock
include/chatwire/
  chatwire.hpp          the public surface; includes neither heavyweight header
  features/*.hpp        behaviour; knows nothing about vmhook
  feature.hpp           the extension point
  pump.hpp              the game-thread queue
  json.hpp  log.hpp     helpers
  ws/*.hpp              RFC 6455 by hand, Winsock only
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

Two files, two lines. Write the module:

```cpp
export module chatwire.features.inventory;
import chatwire.core.feature;

class inventory_feature final : public chatwire::feature
{
    auto name() const noexcept -> std::string_view override { return "inventory"; }
    auto start() noexcept -> bool override { /* install hooks */ return true; }
    auto stop()  noexcept -> void override { }
    auto handle(const chatwire::command& cmd) noexcept -> chatwire::response override
    {
        if (cmd.verb == "list") { return chatwire::response::success(R"({"items":[]})"); }
        return chatwire::response::failure("unknown verb");
    }
};

export namespace chatwire::features::inventory
{
    auto instance() noexcept -> chatwire::feature*
    {
        static inventory_feature f{};
        return &f;
    }
}
```

Then in `chatwire.ixx` / `chatwire_impl.cpp`:

```cpp
import chatwire.features.inventory;                      // 1
registry::add(features::inventory::instance());          // 2
```

`inventory.list` now routes to it. Nothing else changes — not the server, not the dispatcher,
not the protocol.

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

- **Windows.** It is injected into a running process and uses Winsock directly.
- **CMake 3.20+** and any C++23 compiler. Developed against GCC 15.2 (MSYS2); MSVC and Clang
  work too.

No modules, no package manager, no dependencies beyond `ws2_32`. `vmhook` is vendored in `ext/`.

### A note on modules

chatwire was first written as C++26 named modules. That is gone, and the reason is worth
recording: GCC 15 segfaults instantiating vmhook inside a module, neither GCC 15 nor Clang 19
can compile a translation unit that both `import`s a module and includes `<windows.h>`, Clang
crashes outright on a 24k-line header as a module interface, and GCC does not COMDAT
function-local statics of module-attached inline functions. None of that was a defect in this
code, and all of it was time spent on the build rather than on the library.

The layering those constraints forced was worth keeping — one boundary for vmhook, one for
Winsock — and it survives as ordinary include discipline. `sdk.hpp` is still the only header
that includes vmhook; `ws/server.hpp` is still the only one that includes Winsock; and
`chatwire.hpp` includes neither, so a consumer's build pays for neither.

## Tests

```bash
cmake --build build && ./build/chatwire_test_server
```

Covers the halves that run without Minecraft: the RFC 6455 handshake (against the worked
example in the RFC), frame length boundaries at 125/126/65535/65536, rejection of unmasked and
reserved-bit frames, oversized control frames, JSON parsing of hostile input, client reaping,
protocol-violation dropping, clean shutdown, and restart on the same port.

The Minecraft side needs a live game and is verified by injecting.

## Status

Chat works end to end and the server half is tested. The game side is verified by injection
rather than automatically — there is no test harness that runs a Minecraft client.

## Licence

MIT.
