#!/usr/bin/env python3
"""chatwire-mcp — a running Minecraft client, exposed to an AI over MCP.

Model Context Protocol server.  Point Claude Code, Claude Desktop or any other
MCP client at it and the assistant can read the game's chat, say things, list
the players, and — the interesting one — own a slash command in the game and
answer it.

    pip install mcp
    claude mcp add chatwire -- python /path/to/python/mcp_server.py

Claude Desktop, in claude_desktop_config.json:

    {"mcpServers": {"chatwire": {"command": "python",
                                 "args": ["/path/to/python/mcp_server.py"]}}}

===========================================================================
PUSH MEETS REQUEST/RESPONSE
===========================================================================
MCP is request/response: a model calls a tool and gets an answer.  chatwire is
the other thing — it PUSHES, constantly, whenever a line reaches the chat box or
a registered command is typed.  Those two do not meet on their own.

So this server keeps a reader thread and two buffers.  Chat goes into a ring
(the last N lines, oldest dropped), and command invocations go into a queue that
is DRAINED when read.  That difference is deliberate and is the whole design:

  * chat is a VIEW.  Reading it twice should show the same thing, and missing
    some of it while the model was thinking about something else is normal.
  * a command invocation is WORK.  The player typed it and is waiting.  Reading
    it must hand it over exactly once, so two calls cannot both act on it, and
    it must not be silently dropped by the next one arriving.

===========================================================================
WHAT AN ASSISTANT CAN DO WITH THIS, STATED PLAINLY
===========================================================================
Everything here reaches a real game that a real person is playing, so the tools
are named for what they actually do rather than for what they are for.  In
particular `say` is public — every player on the server sees it, under that
person's own name — and `tell` is not.  They are separate tools rather than one
with a flag, because that is the single easiest mistake to make and the most
embarrassing one to make on somebody else's behalf.

===========================================================================
ONE FILE, NO DEPENDENCIES BUT THE SDK
===========================================================================
The WebSocket framing below is written out rather than pulled from a package.
chatwire's whole claim is that its API is a socket and a flat JSON object, and
the eighty lines under `class Socket` are what that claim costs — worth having
in front of you once.  It also means this runs on any Python 3 with only the
MCP SDK installed, which matters for something a user will point an assistant
at rather than develop against.
"""

from __future__ import annotations

import base64
import collections
import json
import os
import socket
import struct
import sys
import threading
from typing import Any

try:
    from mcp.server.fastmcp import FastMCP
except ImportError:                                    # pragma: no cover
    sys.exit("chatwire-mcp needs the MCP SDK:  pip install mcp")

# ---- the protocol's own names -------------------------------------------
PLAYER = "net.minecraft.client.entity.EntityPlayerSP."
SEND = PLAYER + "sendChatMessage"
ADD = PLAYER + "addChatMessage"
PLAYERS = "net.minecraft.world.World.playerEntities"
CHAT_EVENT = "net.minecraft.client.gui.GuiNewChat.printChatMessage"
COMMAND_EVENT = "net.minecraft.client.entity.EntityPlayerSP.sendChatMessage"

#: How many chat lines to remember.  Enough to answer "what just happened"
#: without handing a model an unbounded transcript it has to pay to read.
CHAT_HISTORY = 200

DEFAULT_PORT = int(os.environ.get("CHATWIRE_PORT", "24455"))


# ===========================================================================
# A minimal RFC 6455 client.  See the note at the top on why it is in here.
# ===========================================================================

class Socket:
    def __init__(self, host: str, port: int, timeout: float | None = None) -> None:
        self.sock = socket.create_connection((host, port), timeout=10)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buffer = bytearray()

        key = base64.b64encode(os.urandom(16)).decode()
        self.sock.sendall(
            f"GET / HTTP/1.1\r\nHost: {host}:{port}\r\n"
            f"Upgrade: websocket\r\nConnection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            f"Sec-WebSocket-Version: 13\r\n\r\n".encode())

        response = b""
        while b"\r\n\r\n" not in response:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("the connection closed during the handshake")
            response += chunk
            if len(response) > 16384:
                raise ConnectionError("the handshake response never ended")
        if b" 101 " not in response.split(b"\r\n", 1)[0]:
            raise ConnectionError("that is not a WebSocket server")

        self.sock.settimeout(timeout)

    def send(self, payload: dict) -> None:
        data = json.dumps(payload).encode()
        header = bytearray([0x81])
        if len(data) < 126:
            header.append(0x80 | len(data))
        elif len(data) <= 0xFFFF:
            header.append(0x80 | 126)
            header += struct.pack(">H", len(data))
        else:
            header.append(0x80 | 127)
            header += struct.pack(">Q", len(data))
        mask = os.urandom(4)
        self.sock.sendall(bytes(header) + mask
                          + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))

    def receive(self) -> dict | None:
        while True:
            try:
                opcode, payload = self._frame()
            except (TimeoutError, socket.timeout):
                return None
            if opcode == 0x8:
                raise ConnectionError("chatwire closed the connection")
            if opcode == 0x9:
                self.sock.sendall(bytes([0x8A, 0x80]) + os.urandom(4))
                continue
            if opcode == 0xA or not payload:
                continue
            try:
                return json.loads(payload)
            except ValueError:
                continue

    def _frame(self) -> tuple[int, bytes]:
        data = bytearray()
        opcode = 0x1
        while True:
            first, second = self._exactly(2)
            if first & 0x0F:
                opcode = first & 0x0F
            length = second & 0x7F
            if length == 126:
                length = struct.unpack(">H", self._exactly(2))[0]
            elif length == 127:
                length = struct.unpack(">Q", self._exactly(8))[0]
            if length > 32 * 1024 * 1024:
                raise ConnectionError("frame too large")
            data += self._exactly(length) if length else b""
            if opcode in (0x8, 0x9, 0xA) or first & 0x80:
                return opcode, bytes(data)

    def _exactly(self, count: int) -> bytes:
        while len(self.buffer) < count:
            chunk = self.sock.recv(max(4096, count - len(self.buffer)))
            if not chunk:
                raise ConnectionError("chatwire closed the connection")
            self.buffer += chunk
        out = bytes(self.buffer[:count])
        del self.buffer[:count]
        return out

    def close(self) -> None:
        try:
            self.sock.sendall(bytes([0x88, 0x80]) + os.urandom(4))
        except OSError:
            pass
        self.sock.close()


def strip_colours(text: str) -> str:
    out: list[str] = []
    i = 0
    while i < len(text):
        if text[i] == "§" and i + 1 < len(text):
            i += 2
            continue
        out.append(text[i])
        i += 1
    return "".join(out)


# ===========================================================================
# The connection, its reader thread, and what the reader has collected.
# ===========================================================================

class Game:
    """One lock guards everything.

    The critical sections are all "append to a deque" or "swap a list", so
    contention is not a consideration; correctness is, because the reader thread
    and the tool calls genuinely run at once.
    """

    def __init__(self, port: int = DEFAULT_PORT) -> None:
        self.port = port
        self._lock = threading.Lock()
        self._ws: Socket | None = None
        self._chat: collections.deque[str] = collections.deque(maxlen=CHAT_HISTORY)
        self._commands: list[dict[str, Any]] = []
        self._claimed: set[str] = set()
        # The protocol has no correlation id, so a reply is simply the next
        # reply-shaped message.  Only one command may be in flight at a time,
        # and this is what enforces that -- two tool calls at once would
        # otherwise each take the other's answer.
        self._call_lock = threading.Lock()
        self._replies: collections.deque[dict] = collections.deque()
        self._reply_ready = threading.Condition()

    # ---- connection -------------------------------------------------

    def connect(self) -> Socket:
        """The live connection, opening one if there is not one already.

        A dead connection is REPLACED rather than reported: the game gets
        restarted and chatwire gets re-injected, and a server that answered "not
        connected" forever after the first hiccup would need the human to
        restart their MCP client to recover.  An MCP client is also usually
        started before the game is, which is why nothing connects at import.
        """
        with self._lock:
            if self._ws is not None:
                return self._ws

            # A timeout, so the reader thread wakes regularly and can notice it
            # has been replaced.  Without one it blocks in recv() forever.
            ws = Socket("127.0.0.1", self.port, timeout=0.5)
            self._ws = ws
            self._chat.clear()
            self._commands.clear()
            self._replies.clear()
            claimed = tuple(self._claimed)
            self._claimed.clear()

        threading.Thread(target=self._pump, args=(ws,), daemon=True).start()

        # Re-claimed OUTSIDE the lock: registering is a round trip, and holding
        # the lock across one would block every other tool for its duration.  A
        # reconnect gets a NEW client id, so chatwire has already dropped the
        # old claims -- these are fresh registrations, not repeats.
        for name in claimed:
            try:
                self.register(name)
            except (ConnectionError, OSError, RuntimeError):
                pass
        return ws

    def _drop(self, ws: Socket) -> None:
        with self._lock:
            if self._ws is ws:
                self._ws = None
        with self._reply_ready:
            self._reply_ready.notify_all()      # unblock anyone waiting
        try:
            ws.close()
        except OSError:
            pass

    def _pump(self, ws: Socket) -> None:
        """Sorts everything the socket produces into replies, chat and work."""
        while True:
            with self._lock:
                if self._ws is not ws:
                    return                      # replaced; this one is done
            try:
                message = ws.receive()
            except (ConnectionError, OSError):
                self._drop(ws)
                return
            if message is None:
                continue                        # just a timeout

            if "ok" in message:
                with self._reply_ready:
                    self._replies.append(message)
                    self._reply_ready.notify_all()
                continue

            kind = message.get("type")
            with self._lock:
                if kind == CHAT_EVENT:
                    self._chat.append(message.get("plain", ""))
                elif kind == COMMAND_EVENT:
                    self._commands.append({
                        "command": message.get("command", ""),
                        "args": message.get("args", []),
                        "raw": message.get("raw", ""),
                    })

    # ---- commands ---------------------------------------------------

    def call(self, cmd: str, **arguments: Any) -> dict:
        """Sends one command and waits for its answer.

        Reconnects once if the connection has died under us, because the usual
        reason it has is that the human restarted their game.
        """
        for attempt in (1, 2):
            ws = self.connect()
            try:
                return self._call_once(ws, cmd, arguments)
            except (ConnectionError, OSError):
                self._drop(ws)
                if attempt == 2:
                    raise

    def _call_once(self, ws: Socket, cmd: str, arguments: dict) -> dict:
        with self._call_lock:
            with self._reply_ready:
                self._replies.clear()           # anything older is not ours
            ws.send(dict(cmd=cmd, **arguments))

            with self._reply_ready:
                # A bound, so a tool call cannot hang an MCP client forever if
                # chatwire stops answering.  Ten seconds is far longer than a
                # loopback round trip and short enough to still be an error.
                if not self._reply_ready.wait_for(lambda: bool(self._replies)
                                                  or self._ws is not ws,
                                                  timeout=10.0):
                    raise ConnectionError("chatwire did not answer in 10s")
                if not self._replies:
                    raise ConnectionError("the connection went away")
                reply = self._replies.popleft()

        if not reply.get("ok"):
            raise RuntimeError(reply.get("error", "refused"))
        return reply.get("result", {})

    def register(self, name: str) -> str:
        claimed = self.call("commands.register", name=name)["registered"]
        with self._lock:
            self._claimed.add(claimed)
        return claimed

    def unregister(self, name: str) -> str:
        released = self.call("commands.unregister", name=name)["unregistered"]
        with self._lock:
            self._claimed.discard(released)
        return released

    def recent_chat(self, limit: int) -> list[str]:
        with self._lock:
            lines = list(self._chat)
        return lines[-limit:] if limit > 0 else lines

    def take_commands(self) -> list[dict[str, Any]]:
        """Hands over every pending invocation and forgets them."""
        with self._lock:
            taken, self._commands = self._commands, []
        return taken


GAME = Game()
mcp = FastMCP("chatwire")


# ---- reading ------------------------------------------------------------

@mcp.tool()
def read_chat(limit: int = 40) -> str:
    """Recent lines from the player's Minecraft chat box, oldest first.

    This is everything the PLAYER saw, not everything the server sent: mod
    output, client-side messages and command replies that never crossed the
    network are all in here, because chatwire reads the method that renders
    chat rather than reading the network.

    Only lines since this server connected, up to the last 200.
    """
    GAME.connect()
    lines = GAME.recent_chat(limit)
    return "\n".join(lines) if lines else "No chat seen yet."


@mcp.tool()
def list_players() -> str:
    """Players the client currently has LOADED, with their UUIDs.

    Not the server's roster: these are the players near enough to the local
    player to exist as entities, so on a large server this is a small fraction
    of the tab list.  Do not present it as "who is online".
    """
    players = GAME.call(PLAYERS).get("players", [])
    if not players:
        return "No players loaded (the client may be on the title screen)."
    return "\n".join(f"{p['name']}  {p['uuid']}" for p in players)


@mcp.tool()
def game_status() -> str:
    """chatwire's own state: version, mapping, port and connected clients."""
    status = GAME.call("system.status")
    return "\n".join(f"{key}: {value}" for key, value in status.items())


# ---- writing ------------------------------------------------------------

@mcp.tool()
def say(text: str) -> str:
    """Says TEXT in Minecraft chat, PUBLICLY, as the player.

    Every player on the server sees this, under the player's own name, exactly
    as if they had typed it — including a leading '/' running a real command.
    It cannot be taken back.  If you only want the person playing to see
    something, use `tell` instead.

    Limited to 100 characters, which is Minecraft's own limit.
    """
    GAME.call(SEND, text=text)
    return f"Said publicly: {text}"


@mcp.tool()
def tell(text: str) -> str:
    """Shows TEXT in the player's own chat box.  Nothing is transmitted.

    Nobody else sees it and no server is involved — this draws a line in the
    local client and nothing more.  This is the safe one, and the right choice
    for anything addressed to the person playing.

    Minecraft colour codes work: "§ahello" is green.
    """
    GAME.call(ADD, text=text)
    return f"Shown to the player only: {strip_colours(text)}"


# ---- commands added to the game at runtime ------------------------------

@mcp.tool()
def claim_command(name: str) -> str:
    """Claims /NAME in Minecraft, so that typing it reaches you instead.

    From here on the player typing /NAME does NOT go to the server: chatwire
    swallows the line and holds the invocation for `take_commands`.  Nothing
    answers it until you do — so claim a command only when you intend to poll
    for it, or the player will type it and get silence.

    The claim is dropped if this server loses its connection, and re-made when
    it reconnects.
    """
    claimed = GAME.register(name)
    return (f"/{claimed} is now yours. It no longer reaches the server; call "
            f"take_commands to see invocations, and tell() to answer them.")


@mcp.tool()
def release_command(name: str) -> str:
    """Gives /NAME back, so it reaches the server again."""
    return f"/{GAME.unregister(name)} released; it goes to the server again."


@mcp.tool()
def take_commands() -> str:
    """Invocations of your claimed commands, since the last call.

    DRAINS the queue: each invocation is handed over exactly once, because the
    player typed it and is waiting for an answer, and two calls both acting on
    it would answer twice.  Answer with `tell`.
    """
    GAME.connect()
    taken = GAME.take_commands()
    if not taken:
        return "Nothing typed."
    return "\n".join(f"/{one['command']}  args={one['args']}  "
                     f"raw={one['raw']!r}" for one in taken)


@mcp.tool()
def list_claimed_commands() -> str:
    """Every command claimed through chatwire, and which client owns it."""
    claimed = GAME.call("commands.list").get("commands", [])
    if not claimed:
        return "No commands are registered."
    return "\n".join(f"/{one['name']}  (client {one['client']})"
                     for one in claimed)


if __name__ == "__main__":
    mcp.run()
