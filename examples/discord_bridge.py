#!/usr/bin/env python3
"""Bridge one or more Minecraft clients to a Discord channel.

Every chat line a player sees goes to Discord; every message in the channel is
said in the game.  Point it at several chatwire endpoints and the players see
each other's chat too, which is the whole reason this example exists: the two
games can be on different machines on different networks.

    pip install discord.py websockets
    set DISCORD_TOKEN=...
    set DISCORD_CHANNEL=123456789012345678
    set CHATWIRE_NODES=alice=ws://127.0.0.1:24455,bob=ws://10.6.0.3:24455
    set CHATWIRE_SECRET=correct horse battery staple
    python discord_bridge.py

CHATWIRE_NODES is `label=url` pairs.  The label is what Discord sees in front of
a line, so it should say which game it came from.

===========================================================================
ABOUT REACHING ANOTHER MACHINE
===========================================================================
chatwire authenticates but does NOT encrypt.  A node on another network must be
reached through a tunnel -- WireGuard, Tailscale, `ssh -L 24455:127.0.0.1:24455`
-- and then the URL here is the tunnel's address.  The example above uses
10.6.0.3, a WireGuard peer, on purpose.

Do not put a chatwire on a public IP and rely on the token alone.  The token
stops someone from driving the game; it does nothing about someone reading every
word the player types, because the frames are plaintext.

===========================================================================
WHY IT LOOKS LIKE THIS
===========================================================================
Two things it would be easy to get wrong and which matter more than the size of
the file:

  * A CHAT LINE THAT CAME FROM DISCORD MUST NOT GO BACK TO DISCORD.  chatwire
    reports every line the player sees, including the ones this bridge just
    said, so without a guard two nodes echo each other forever and the channel
    fills in seconds.  `recently_sent` is that guard.
  * `§` GETS THE PLAYER KICKED.  Minecraft's colour codes are legal to receive
    and illegal to send: a client that transmits one is disconnected by the
    server for it.  Everything on its way into the game is stripped.
"""

from __future__ import annotations

import asyncio
import collections
import hashlib
import hmac
import json
import os
import re
import time

import discord
import websockets

CHAT_EVENT = "net.minecraft.client.gui.GuiNewChat.printChatMessage"
SEND = "net.minecraft.client.entity.EntityPlayerSP.sendChatMessage"
TAB = "net.minecraft.client.network.NetHandlerPlayClient.getPlayerInfoMap"

#: Minecraft's colour codes: legal to receive, and a kick if you send one.
SECTION = re.compile(r"§.")
#: Minecraft refuses a chat line longer than this, so it is cut rather than lost.
MAX_CHAT = 100


def nodes_from_env() -> dict[str, str]:
    raw = os.environ.get("CHATWIRE_NODES", "game=ws://127.0.0.1:24455")
    out: dict[str, str] = {}
    for pair in raw.split(","):
        label, _, url = pair.partition("=")
        if url:
            out[label.strip()] = url.strip()
    return out


def clean_for_minecraft(text: str) -> str:
    return SECTION.sub("", text).replace("\n", " ")[:MAX_CHAT]


class Node:
    """One chatwire connection, with the reconnect loop around it."""

    def __init__(self, label: str, url: str, secret: str, to_discord) -> None:
        self.label = label
        self.url = url
        self.secret = secret
        self.to_discord = to_discord
        self.socket: websockets.WebSocketClientProtocol | None = None
        # THE READER OWNS THE SOCKET.  Only run() may call recv(): a second
        # coroutine reading the same WebSocket is a RuntimeError, and it is the
        # obvious way to write `who()` -- send the command, read the reply.  The
        # reader hands replies over through this future instead.
        self.pending: asyncio.Future | None = None
        # Lines this bridge said in the game.  chatwire reports them back like
        # any other, and without this the nodes echo each other forever.
        self.recently_sent: collections.deque[tuple[float, str]] = collections.deque(maxlen=64)

    def _is_our_echo(self, plain: str) -> bool:
        now = time.time()
        while self.recently_sent and now - self.recently_sent[0][0] > 10:
            self.recently_sent.popleft()
        return any(text in plain for _, text in self.recently_sent)

    async def _authenticate(self, ws) -> bool:
        """Answer the challenge, if there is one.

        A node with no token sends no challenge, so the first frame is an
        ordinary event and must not be swallowed.  It is put back by handling it
        here rather than by peeking, because a WebSocket has no peek.
        """
        if not self.secret:
            return True
        frame = json.loads(await asyncio.wait_for(ws.recv(), timeout=20))
        if frame.get("type") != "chatwire.auth.challenge":
            print(f"[{self.label}] expected a challenge, got {frame}")
            return False
        proof = hmac.new(self.secret.encode(), frame["nonce"].encode(),
                         hashlib.sha256).hexdigest()
        await ws.send(json.dumps({"cmd": "system.auth", "proof": proof}))
        reply = json.loads(await asyncio.wait_for(ws.recv(), timeout=20))
        if not reply.get("ok"):
            print(f"[{self.label}] authentication refused: {reply.get('error')}")
            return False
        return True

    async def say(self, text: str) -> None:
        if self.socket is None:
            return
        line = clean_for_minecraft(text)
        if not line:
            return
        self.recently_sent.append((time.time(), line))
        try:
            await self.socket.send(json.dumps({"cmd": SEND, "text": line}))
        except Exception as e:                                 # noqa: BLE001
            print(f"[{self.label}] send failed: {e}")

    async def ask(self, request: dict, timeout: float = 15.0) -> dict | None:
        """Send a command and wait for its reply, without touching recv().

        One request at a time, which is all this bridge ever needs.  A second
        caller would overwrite `pending`, so it is refused rather than raced.
        """
        if self.socket is None or self.pending is not None:
            return None
        self.pending = asyncio.get_running_loop().create_future()
        try:
            await self.socket.send(json.dumps(request))
            return await asyncio.wait_for(self.pending, timeout=timeout)
        except Exception:                                      # noqa: BLE001
            return None
        finally:
            self.pending = None

    async def who(self) -> list[str]:
        reply = await self.ask({"cmd": TAB})
        if not reply or not reply.get("ok"):
            return []
        return [p["name"] for p in reply["result"]["players"]]

    async def run(self) -> None:
        while True:
            try:
                async with websockets.connect(self.url, open_timeout=20) as ws:
                    if not await self._authenticate(ws):
                        await asyncio.sleep(30)
                        continue
                    self.socket = ws
                    print(f"[{self.label}] connected to {self.url}")
                    await self.to_discord(f"**{self.label}** bridge connected")

                    async for raw in ws:
                        event = json.loads(raw)
                        # A reply, handed to whoever is waiting for one.
                        if "ok" in event:
                            if self.pending is not None and not self.pending.done():
                                self.pending.set_result(event)
                            continue
                        if event.get("type") != CHAT_EVENT:
                            continue
                        plain = event.get("plain", "").strip()
                        if not plain or self._is_our_echo(plain):
                            continue
                        await self.to_discord(f"`{self.label}` {plain}")
            except Exception as e:                             # noqa: BLE001
                print(f"[{self.label}] disconnected: {e}")
            finally:
                self.socket = None
            await asyncio.sleep(5)


class Bridge(discord.Client):
    def __init__(self, channel_id: int, nodes: dict[str, str], secret: str) -> None:
        super().__init__(intents=discord.Intents(guilds=True, guild_messages=True,
                                                 message_content=True))
        self.channel_id = channel_id
        self.nodes = [Node(label, url, secret, self.to_discord)
                      for label, url in nodes.items()]

    async def to_discord(self, text: str) -> None:
        channel = self.get_channel(self.channel_id)
        if channel is not None:
            # 2000 is Discord's limit; a long scoreboard line reaches it.
            await channel.send(text[:1990])

    async def setup_hook(self) -> None:
        for node in self.nodes:
            self.loop.create_task(node.run())

    async def on_message(self, message: discord.Message) -> None:
        if message.author.bot or message.channel.id != self.channel_id:
            return

        if message.content.strip() == "!who":
            for node in self.nodes:
                names = await node.who()
                await self.to_discord(
                    f"`{node.label}` {len(names)} online: {', '.join(sorted(names)) or '-'}")
            return

        said = f"[{message.author.display_name}] {message.content}"
        for node in self.nodes:
            await node.say(said)


def main() -> None:
    token = os.environ.get("DISCORD_TOKEN")
    channel = os.environ.get("DISCORD_CHANNEL")
    if not token or not channel:
        raise SystemExit("set DISCORD_TOKEN and DISCORD_CHANNEL")

    Bridge(int(channel), nodes_from_env(),
           os.environ.get("CHATWIRE_SECRET", "")).run(token)


if __name__ == "__main__":
    main()
