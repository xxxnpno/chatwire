# chatwire

A live WebSocket API into a running **Minecraft 1.8.9** client. Inject it, connect a WebSocket,
and read and drive the game from any language over a socket.

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
│              │  events out, commands in   │   ├─ hooks GuiNewChat.printChatMessage │
│              │                            │   └─ calls Java on the asking thread   │
└──────────────┘                            └────────────────────────────────────────┘
```

Your command runs on **your own socket thread**, which calls into Java directly and answers with
what happened. There is no queue and no tick to wait for — see
[Calling the game from a socket thread](#calling-the-game-from-a-socket-thread).

## What it does today

| Direction | Command | What |
|---|---|---|
| **game → you** | — | every line that reaches the chat box, with and without colour codes |
| **you → game** | `net.minecraft.client.entity.EntityPlayerSP.sendChatMessage` | say it to the server, exactly as if typed |
| **you → game** | `net.minecraft.client.entity.EntityPlayerSP.addChatMessage` | show it only to this client, never transmitted |
| **you → game** | `net.minecraft.world.World.playerEntities` | every player the client has loaded, with name and UUID |

`chat` and `world` are features; `system` adds `status`, `stats`, `ping` and `detach`. Adding
another is a new file and one line — `world` was added exactly that way, and nothing in the server,
the dispatcher or the protocol changed to make it work.

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

**Per-feature coverage, honestly.** `chat` and `system` carry all three mappings. `world` carries
MCP and SRG but not OBF — the obfuscated names for `playerEntities`, `getName` and `getUniqueID`
are not in the table, so on a raw vanilla jar that command fails cleanly rather than guessing. Add
them to `mapping.hpp` and it works; a wrong guess there would call the wrong method, which is worse
than an honest failure.

## Quick start

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

You get:

| Binary | What it is |
|---|---|
| `chatwire.dll` / `.so` / `.dylib` | the library that goes into Minecraft |
| `chatwire-inject` | **Windows only** — finds a running Minecraft and injects the library |
| `chatwire-preload` | **Linux and macOS** — starts the game with the library already in it |
| `chatwire-client` | a terminal chat client — the reference consumer |
| `chatwire-mock` | serves the protocol with **fake** chat, so you can build against it with no game running |

On Windows, start Minecraft and then inject:

```
> chatwire-inject
chatwire-inject

  dll   : C:\...\chatwire.dll
  target: pid 18244

  injecting...

  injected.  connect to  ws://127.0.0.1:24455
```

On Linux and macOS, start the game *through* chatwire instead:

```bash
./build/chatwire-preload -- java -jar launcher.jar
```

Either way, you now have a socket:

```
> chatwire-client
connected.  anything you type goes to
  net.minecraft.client.entity.EntityPlayerSP.sendChatMessage

  /net.minecraft.client.entity.EntityPlayerSP.addChatMessage <text>
      client-side only; nobody else sees it
  /net.minecraft.world.World.playerEntities
      every player this client has loaded, with name and UUID
  /system.status  /system.stats  /system.ping  /system.detach  /quit

[Team] Steve: hello there              ← lines from the game, in colour
Alex joined the game
hey everyone                           ← you typed this; it went to the server

> /net.minecraft.client.entity.EntityPlayerSP.addChatMessage just for me
                                       ← drawn in your chat box, transmitted nowhere

> /net.minecraft.world.World.playerEntities
  Steve             8667ba71-b85a-4004-af54-457a9734eed7
  Alex              ec561538-f3fd-461d-aff5-086b22154bce
  2 player(s) loaded by this client
```

Yes, they are long. The client has **no aliases of its own** — what you type at that prompt is
exactly what you would put in a `cmd` field from any other language, which is the only thing a
reference client is for. A private vocabulary would teach you its vocabulary instead of the API's.

## Getting chatwire into the game

This is the one place the three platforms genuinely differ, and the difference is not a gap in
the port — it is what each operating system is willing to let one process do to another.

| | Windows | Linux | macOS |
|---|---|---|---|
| Attach to a game **already running** | ✅ `chatwire-inject` | ❌ | ❌ |
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
same reason ([below](#stability)). On Windows `chatwire-inject` can start the resident copy again,
because it has a named event to set. On Linux and macOS there is nothing to set it *with*: a
second `LD_PRELOAD` is not something that can happen to a running process, and `dlopen`ing the
library again would only bump a reference count without re-running its initialiser. So there,
starting again means restarting the game.

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

const CHAT_LINE = "net.minecraft.client.gui.GuiNewChat.printChatMessage";
const PLAYER    = "net.minecraft.client.entity.EntityPlayerSP.";

ws.onmessage = (e) => {
    const msg = JSON.parse(e.data);
    if (msg.type === CHAT_LINE) console.log(msg.plain);            // every line in chat
};

// say it to the server, as if typed
ws.send(JSON.stringify({ cmd: PLAYER + "sendChatMessage", text: "hello world" }));

// show it only to me
ws.send(JSON.stringify({ cmd: PLAYER + "addChatMessage", text: "§athis is client-side only" }));

// who the client has loaded
ws.send(JSON.stringify({ cmd: "net.minecraft.world.World.playerEntities" }));
```

Bind the names to constants once, as above, and the length stops mattering — you write them a
single time and get to see, at that one place, exactly which member of which class each call
reaches.

The port is `24455` by default, or set `CHATWIRE_PORT` in the game's environment.

## Protocol

Every message is one flat JSON object. A command is `<class>.<member>`, split at the **last** dot.

**Pushed to you, unprompted:**

```json
{"type":"net.minecraft.client.gui.GuiNewChat.printChatMessage",
 "formatted":"§b[Team] §fhi","plain":"[Team] hi"}
```

`formatted` keeps the `§` colour codes; `plain` has them stripped. Use whichever suits.

**Sent by you:**

| Command | Arguments | Effect |
|---|---|---|
| `net.minecraft.client.entity.EntityPlayerSP.sendChatMessage` | `text` (≤100 chars) | Sends to the server. A leading `/` runs a command. Reaches `sendChatMessage(String)`. |
| `net.minecraft.client.entity.EntityPlayerSP.addChatMessage` | `text` | Client-side only. Nobody else sees it. Reaches `addChatMessage(IChatComponent)`. |
| `net.minecraft.world.World.playerEntities` | — | Every player the client has loaded, each with `name` and `uuid`. Reads the field, plus `Entity.getName()` / `getUniqueID()`. |
| `system.status` | — | Version, mapping, bound port, connected clients, and `can_call` (whether this JVM lets chatwire reach the game). |
| `system.stats` | — | Counters: lines seen, messages sent, messages added. |
| `system.ping` | — | Liveness. |
| `system.detach` | — | **Unloads chatwire from the game.** |

Every command is the member it reaches, and the pushed event is the method it comes out of. If you
have read Minecraft's source, you already know what each one does — and, more usefully, you know
the difference between the two chat ones.

Three names moved when the short spellings went, and old clients will notice:

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
from. Everything observable is gone either way: socket closed, hooks out. Re-running
`chatwire-inject` starts the resident copy again.

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
  console.hpp           chatwire's own console window, and its commands
  ansi.hpp   config.hpp colour codes; port and options from the environment
  json.hpp   log.hpp    helpers
  ws/*.hpp              RFC 6455 by hand; no platform header of its own
  net.hpp               ◄── THE ONLY HEADER THAT KNOWS SOCKETS DIFFER
  module.hpp            ◄── and the only one that knows how to find itself
  terminal.hpp          ANSI and UTF-8, for the standalone tools
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

Configure with `-DCHATWIRE_BUILD_TOOLS=OFF` to build just the library, which drops the `<print>`
requirement entirely.

### What is verified where

Being honest about this, because "cross-platform" is easy to claim and easy to get wrong:

| | Builds & server tests | Reference client ↔ mock | Injected into a real Minecraft |
|---|---|---|---|
| Windows | ✅ CI + locally | ✅ CI + locally | ✅ by hand |
| Linux | ✅ CI | ✅ CI, and the library is loaded via `LD_PRELOAD` to prove it resolves | ❌ **not yet** |
| macOS | ✅ CI | ✅ CI, same | ❌ **not yet** |

The Minecraft half — mappings, hooks, every Java call — needs a running game and is verified by
injecting into one. That has been done on Windows. The POSIX loader path, the sockets and the
protocol are covered by [CI](.github/workflows/ci.yml) on all three; what has *not* been done is
attaching to a real Minecraft on Linux or macOS and watching chat arrive. If you try it, the
mapping detection is the part most likely to surprise us.

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
protocol-violation dropping, clean shutdown, and restart on the same port.

[CI](.github/workflows/ci.yml) runs all of that on Windows, Linux and macOS, then runs the
reference client against the mock over a real socket, and on POSIX loads the library with
`LD_PRELOAD` / `DYLD_INSERT_LIBRARIES` — which is the cheapest way to catch the failure a
statically-linked Windows build would never reveal, an unresolved symbol in the shared object.

The Minecraft side needs a live game and is verified by injecting.  See
[What is verified where](#what-is-verified-where).

## Status

Chat works end to end and the server half is tested on all three platforms. The game side is
verified by injection rather than automatically — there is no test harness that runs a Minecraft
client — and that injection has been done on **Windows**. Linux and macOS build, serve the
protocol and load into a host process in CI, but have not yet been pointed at a real game. See
[What is verified where](#what-is-verified-where).

## Licence

MIT.
