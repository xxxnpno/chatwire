#!/usr/bin/env python3
"""Tests for the MCP server, against a stub that speaks chatwire's protocol.

    python tests/test_python.py

WHY THERE IS A STUB SERVER IN HERE
----------------------------------
chatwire's C++ suite covers the C++ half — the RFC 6455 codec, the JSON, the
targeted delivery a plugin's events ride on.  None of that exercises the MCP
server, which is the one program here written in another language, and which
contains the thing most likely to be quietly wrong: a hand-written WebSocket
codec plus a reader thread sorting pushed events away from replies.

The stub below is a test fixture and nothing else.  It is not a program anybody
runs and it is not shipped — the point is specifically NOT to have a second
implementation of chatwire sitting around telling comfortable lies.  It asserts
the MCP server's behaviour against the wire format; where the wire format itself
is in question, the C++ suite is what settles it.

mcp_server is imported as a module, which runs nothing: it guards its entry
point with `if __name__ == "__main__"`.  The whole file skips when the MCP SDK
is not installed, since that is its only dependency.
"""

from __future__ import annotations

import base64
import hashlib
import importlib.util
import json
import os
import socket
import struct
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "python"))

_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

failures = 0


def check(name: str, ok: bool) -> None:
    global failures
    print(f"[{'PASS' if ok else 'FAIL'}] {name}")
    if not ok:
        failures += 1


class StubServer(threading.Thread):
    """Just enough of chatwire to answer one client."""

    def __init__(self) -> None:
        super().__init__(daemon=True)
        self.listener = socket.socket()
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(1)
        self.port = self.listener.getsockname()[1]
        self.seen: list[dict] = []                  # every command the client sent
        self.client: socket.socket | None = None
        self.ready = threading.Event()
        self._buffer = bytearray()

    # ---- framing ----------------------------------------------------

    def _recv(self, n: int) -> bytes:
        assert self.client is not None
        while len(self._buffer) < n:
            chunk = self.client.recv(65536)
            if not chunk:
                raise ConnectionError
            self._buffer += chunk
        out = bytes(self._buffer[:n])
        del self._buffer[:n]
        return out

    def read_frame(self) -> tuple[int, bytes]:
        first, second = self._recv(2)
        opcode = first & 0x0F
        masked = bool(second & 0x80)
        length = second & 0x7F
        if length == 126:
            length = struct.unpack(">H", self._recv(2))[0]
        elif length == 127:
            length = struct.unpack(">Q", self._recv(8))[0]
        # A CLIENT frame must be masked, and this asserts it rather than
        # tolerating it: an unmasked client frame is a protocol violation the
        # real server drops the connection over, so a client that stopped
        # masking would pass a lenient test and fail against the game.
        assert masked, "client frame was not masked"
        mask = self._recv(4)
        payload = self._recv(length) if length else b""
        return opcode, bytes(b ^ mask[i % 4] for i, b in enumerate(payload))

    def push(self, payload: dict) -> None:
        """Sends one UNMASKED frame, as a server must."""
        assert self.client is not None
        data = json.dumps(payload).encode()
        header = bytearray([0x81])
        if len(data) < 126:
            header.append(len(data))
        elif len(data) <= 0xFFFF:
            header.append(126)
            header += struct.pack(">H", len(data))
        else:
            header.append(127)
            header += struct.pack(">Q", len(data))
        self.client.sendall(bytes(header) + data)

    # ---- the conversation -------------------------------------------

    def run(self) -> None:
        self.client, _ = self.listener.accept()

        request = b""
        while b"\r\n\r\n" not in request:
            request += self.client.recv(4096)

        key = ""
        for line in request.decode("latin1").split("\r\n"):
            if line.lower().startswith("sec-websocket-key:"):
                key = line.split(":", 1)[1].strip()
        accept = base64.b64encode(
            hashlib.sha1((key + _GUID).encode()).digest()).decode()
        self.client.sendall(
            f"HTTP/1.1 101 Switching Protocols\r\n"
            f"Upgrade: websocket\r\nConnection: Upgrade\r\n"
            f"Sec-WebSocket-Accept: {accept}\r\n\r\n".encode())
        self.ready.set()

        try:
            while True:
                opcode, payload = self.read_frame()
                if opcode == 0x8:
                    return
                if opcode != 0x1:
                    continue
                message = json.loads(payload)
                self.seen.append(message)
                self.push(self.answer(message))
        except (ConnectionError, OSError, ValueError, AssertionError):
            return

    def answer(self, message: dict) -> dict:
        cmd = message.get("cmd", "")
        if cmd.endswith("sendChatMessage"):
            if len(message.get("text", "")) > 100:
                return {"ok": False,
                        "error": "'text' exceeds the 100-character chat limit"}
            return {"ok": True, "result": {"sent": True}}
        if cmd.endswith("addChatMessage"):
            return {"ok": True, "result": {"added": True}}
        if cmd == "net.minecraft.world.World.playerEntities":
            return {"ok": True, "result": {"count": 1, "players": [
                {"name": "Steve",
                 "uuid": "8667ba71-b85a-4004-af54-457a9734eed7"}]}}
        if cmd == "commands.register":
            return {"ok": True, "result": {"registered": message["name"]}}
        if cmd == "commands.unregister":
            return {"ok": True, "result": {"unregistered": message["name"]}}
        if cmd == "commands.list":
            return {"ok": True, "result": {"count": 0, "commands": []}}
        if cmd == "system.ping":
            return {"ok": True, "result": {"pong": True}}
        return {"ok": False, "error": f"no feature named '{cmd}'"}


def test_framing(server: StubServer, mcp_server) -> None:
    """The WebSocket codec, which is the part most likely to be quietly wrong."""
    ws = mcp_server.Socket("127.0.0.1", server.port, timeout=2.0)
    server.ready.wait(5)

    ws.send({"cmd": mcp_server.SEND, "text": "hello everyone"})
    reply = ws.receive()
    check("send_reaches_the_server", reply["ok"] is True)
    check("send_uses_the_full_java_member",
          server.seen[-1]["cmd"] == mcp_server.SEND)
    check("send_and_add_are_different_commands", mcp_server.SEND != mcp_server.ADD)

    ws.send({"cmd": mcp_server.ADD, "text": "§aonly me"})
    check("add_is_answered", ws.receive()["result"] == {"added": True})

    # register takes `name`, not `text`.  Sending it as `text` is the mistake a
    # client written from the chat commands alone would make, and the real
    # server refuses it -- so it is worth pinning here.
    ws.send({"cmd": "commands.register", "name": "ping"})
    check("register_is_accepted",
          ws.receive()["result"] == {"registered": "ping"})
    check("register_sends_name_not_text",
          "name" in server.seen[-1] and "text" not in server.seen[-1])

    ws.send({"cmd": mcp_server.SEND, "text": "x" * 101})
    refusal = ws.receive()
    check("an_over_long_message_is_refused", refusal["ok"] is False)
    check("the_refusal_says_why", "100-character" in refusal["error"])

    # An error is an ANSWER, not a broken socket: a client that desynchronised
    # here would fail every later call for no visible reason.
    ws.send({"cmd": "system.ping"})
    check("the_connection_survives_a_refusal",
          ws.receive()["result"] == {"pong": True})

    # The length boundaries, in BOTH directions.  125/126 and 65535/65536 are
    # where the encoding changes, and a hand-written codec that is wrong is
    # almost always wrong exactly here.
    for size in (125, 126, 65535, 65536):
        server.push({"type": mcp_server.CHAT_EVENT, "plain": "x" * size,
                     "formatted": ""})
        check(f"a_{size}_byte_frame_is_read", len(ws.receive()["plain"]) == size)

    for size in (125, 126, 65535, 65536):
        ws.send({"cmd": mcp_server.ADD, "text": "y" * size})
        ws.receive()
        check(f"a_{size}_byte_frame_is_written",
              len(server.seen[-1]["text"]) == size)

    ws.close()


def test_mcp(server: StubServer, mcp_server) -> None:
    """The reader thread, and what it does with each kind of message."""
    game = mcp_server.Game(port=server.port)
    check("mcp_connects_and_answers", game.call("system.ping") == {"pong": True})

    check("strip_colours_removes_codes",
          mcp_server.strip_colours("§a[Team] §fhi") == "[Team] hi")
    # A trailing lone section sign has no code after it; reading one would step
    # past the end of the string.
    check("strip_colours_survives_a_trailing_section",
          mcp_server.strip_colours("hi§") == "hi§")

    # The reader must sort a pushed event away from a reply.  This is the race
    # that would otherwise make a tool call return a chat line as its result.
    server.push({"type": mcp_server.CHAT_EVENT, "plain": "hello there",
                 "formatted": ""})
    time.sleep(0.3)
    check("mcp_still_answers_with_an_event_queued",
          game.call("system.ping") == {"pong": True})
    check("mcp_kept_the_chat_line", game.recent_chat(10) == ["hello there"])

    # A command invocation is WORK: handed over exactly once, so two readers
    # cannot both act on it and answer the player twice.
    server.push({"type": mcp_server.COMMAND_EVENT, "command": "ping",
                 "args": ["alpha", "beta"], "raw": "/ping alpha beta"})
    time.sleep(0.3)
    taken = game.take_commands()
    check("mcp_takes_the_invocation",
          len(taken) == 1 and taken[0]["args"] == ["alpha", "beta"])
    check("mcp_carries_the_raw_line", taken[0]["raw"] == "/ping alpha beta")
    check("mcp_drains_the_queue", game.take_commands() == [])
    # Chat is a VIEW, so the opposite must hold: reading it again shows it.
    check("mcp_does_not_drain_chat", game.recent_chat(10) == ["hello there"])

    check("mcp_register_returns_the_name", game.register("ping") == "ping")
    check("mcp_remembers_what_it_claimed", "ping" in game._claimed)
    check("mcp_unregister_forgets_it",
          game.unregister("ping") == "ping" and "ping" not in game._claimed)

    try:
        game.call("nonsense.verb")
        check("mcp_raises_on_a_refusal", False)
    except RuntimeError:
        check("mcp_raises_on_a_refusal", True)


def main() -> int:
    if importlib.util.find_spec("mcp") is None:
        print("[SKIP] the MCP SDK is not installed (pip install mcp)")
        return 0

    import mcp_server

    framing = StubServer()
    framing.start()
    test_framing(framing, mcp_server)
    framing.listener.close()

    reader = StubServer()
    reader.start()
    test_mcp(reader, mcp_server)
    reader.listener.close()

    print("\n" + ("ALL PASSED" if failures == 0 else f"{failures} FAILED"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
