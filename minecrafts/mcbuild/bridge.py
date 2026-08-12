"""Run chatwire against each mapping and report what it found.

This is what the three clients are FOR.  chatwire attaches to a running JVM and
reads whatever names that jar happens to carry, so the only honest test of its
name table is to attach it to each of the three and ask.

Per mapping it launches the client, waits for the main menu, injects
`build/chatwire.exe --pid <java>`, connects a WebSocket, and asks:

    system.status      which mapping chatwire decided it was in
    mapping.detected   the four probes that decision was made from
    mapping.verify     every name in the table, checked against this JVM
    mapping.resolve    one name, translated into this jar's spelling

Then it detaches and stops the client.  A mapping whose `missing` count is not
zero fails the run, which is the whole point: that number was unobservable
before, and a wrong OBF name spent a release being invisible because of it.
"""

from __future__ import annotations

import asyncio
import json
import socket


import subprocess
import time
from pathlib import Path

from . import launch as launch_mod
from . import paths
from . import server as server_mod
from .util import say

#: One port each, so `--keep` can leave all three up at once.
PORTS = {"vanilla": 24455, "srg": 24456, "mcp": 24457}

#: What the run asks for, in order.  A verb whose reply is only interesting in
#: part is summarised by `_summarise` rather than dumped whole.
QUERIES: list[dict] = [
    {"cmd": "system.status"},
    {"cmd": "mapping.detected"},
    {"cmd": "mapping.verify"},
    {"cmd": "mapping.resolve", "name": "printChatMessage"},
]

#: Asked only once the client is in a world, because that is the only place
#: they mean anything.
IN_WORLD_QUERIES: list[dict] = [
    {"cmd": "net.minecraft.client.Minecraft.thePlayer"},
    {"cmd": "net.minecraft.client.Minecraft.currentScreen"},
    {"cmd": "net.minecraft.world.World.playerEntities"},
]


def injector() -> Path:
    exe = paths.ROOT.parent / "build" / "chatwire.exe"
    if not exe.is_file():
        raise SystemExit(f"{exe} is missing -- build chatwire first")
    return exe


def wait_for_menu(mapping: str, proc: subprocess.Popen, timeout: float = 180.0) -> bool:
    """Block until this client's log says the main menu is up.

    `proc.poll()`, never `os.kill(pid, 0)`: on Windows Python ignores the signal
    number and calls TerminateProcess, so the obvious liveness check kills the
    process it is asking about.  It did, for one run of this file.
    """
    log = paths.mapping_run(mapping) / "launch.log"
    deadline = time.time() + timeout
    while time.time() < deadline:
        if log.is_file():
            text = log.read_text(encoding="utf-8", errors="replace")
            if any(m in text for m in launch_mod.READY_MARKERS):
                return True
        if proc.poll() is not None:
            return False
        time.sleep(0.5)
    return False


def wait_in_world(port: int, timeout: float = 120.0) -> bool:
    """Poll `thePlayer` until it stops saying "not in a world".

    Joining is not instant and the client log does not say when it is done in a
    way worth parsing.  chatwire already answers the question exactly -- the
    command fails until there is a player -- so the harness asks the bridge it
    is testing rather than guessing from a log line.
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            reply = ask(port, [{"cmd": "net.minecraft.client.Minecraft.thePlayer"}])
            if reply and reply[0]["reply"].get("ok"):
                return True
        except Exception:                                      # noqa: BLE001
            pass
        time.sleep(2)
    return False


def wait_for_port(port: int, timeout: float = 90.0) -> bool:
    """Wait until something is listening on 127.0.0.1:`port`.

    chatwire binds only after it has found Minecraft's classes and started its
    features, and how long that takes depends on the client rather than on us.
    A fixed sleep here was wrong on the first run of three clients at once and
    would have been wrong again on a slower machine for a different reason.
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.settimeout(1.0)
            if s.connect_ex(("127.0.0.1", port)) == 0:
                return True
        time.sleep(1.0)
    return False


async def _ask(port: int, queries: list[dict]) -> list[dict]:
    import websockets

    out: list[dict] = []
    async with websockets.connect(f"ws://127.0.0.1:{port}", open_timeout=20) as ws:
        for q in queries:
            await ws.send(json.dumps(q))
            # The server also pushes events (chat lines, world changes) down the
            # same socket.  A reply is the frame carrying "ok"; anything else is
            # an event that happened to arrive between the ask and the answer.
            while True:
                frame = json.loads(await asyncio.wait_for(ws.recv(), timeout=30))
                if "ok" in frame:
                    out.append({"query": q, "reply": frame})
                    break
    return out


def ask(port: int, queries: list[dict]) -> list[dict]:
    return asyncio.run(_ask(port, queries))


def _summarise(mapping: str, exchanges: list[dict]) -> int:
    """Print one client's answers.  Returns how many names were absent."""
    missing = 0
    for item in exchanges:
        verb = item["query"]["cmd"]
        reply = item["reply"]
        if not reply.get("ok"):
            say(f"    {verb:<18} FAILED: {reply.get('error') or reply}")
            missing += 1
            continue
        result = reply.get("result", {})

        if verb == "system.status":
            up = ",".join(f["name"] for f in result.get("features", []) if f.get("started"))
            waiting = ",".join(f["name"] for f in result.get("features", [])
                               if not f.get("started"))
            say(f"    {verb:<18} mapping={result.get('mapping')!r} "
                f"port={result.get('port')} can_call={result.get('can_call')}")
            say(f"    {'features':<18} up: {up or '-'}   waiting: {waiting or '-'}")
        elif verb == "mapping.detected":
            say(f"    {verb:<18} {result.get('mapping')!r} "
                f"(minecraft_class={result.get('minecraft_class')} "
                f"mcp_field={result.get('mcp_field')} "
                f"srg_field={result.get('srg_field')} "
                f"obf_class={result.get('obf_class')})")
        elif verb == "mapping.verify":
            missing += int(result.get("missing", 0))
            say(f"    {verb:<18} checked={result.get('checked')} "
                f"missing={result.get('missing')} "
                f"unchecked={result.get('unchecked')}")
            for e in result.get("entries", []):
                if e.get("kind") == "absent":
                    say(f"      ABSENT     {e['group']}.{e['member']} -> {e['spelling']!r}")
                elif e.get("kind") == "not loaded":
                    say(f"      not loaded {e['group']} ({e['spelling']})")
        elif verb == "mapping.resolve":
            say(f"    {verb:<18} {item['query']['name']!r} -> {result.get('spelling')!r} "
                f"({result.get('kind')}, on {result.get('group')})")
        elif verb.endswith(".thePlayer"):
            say(f"    {'thePlayer':<18} {result.get('name')!r} at "
                f"({result.get('x'):.2f}, {result.get('y'):.2f}, {result.get('z'):.2f}) "
                f"yaw={result.get('yaw'):.1f} ground={result.get('on_ground')} "
                f"hp={result.get('health')} food={result.get('food')} "
                f"sat={result.get('saturation')} dim={result.get('dimension')}")
        elif verb.endswith(".currentScreen"):
            say(f"    {'currentScreen':<18} open={result.get('open')} "
                f"{result.get('screen')!r}")
        elif verb.endswith(".playerEntities"):
            names = ", ".join(sorted(p["name"] for p in result.get("players", [])))
            say(f"    {'playerEntities':<18} {result.get('count')} loaded: {names}")
        elif verb.endswith(".sendChatMessage"):
            say(f"    {'sendChatMessage':<18} sent={result.get('sent')}")
        else:
            say(f"    {verb:<18} {result}")
    return missing


def inject(mapping: str, proc: subprocess.Popen, port: int) -> bool:
    say(f"    {mapping}: injecting into pid {proc.pid} on port {port}")
    result = subprocess.run(
        [str(injector()), "--pid", str(proc.pid), "--port", str(port)],
        capture_output=True, text=True, errors="replace")
    if result.returncode != 0:
        say(f"    {mapping}: injection failed ({result.returncode})")
        say("      " + (result.stdout or result.stderr or "").strip()[:800])
        return False
    return True


def stop(proc: subprocess.Popen) -> None:
    try:
        proc.terminate()
    except OSError:
        pass


async def _add_chat_echo(port: int, text: str) -> bool:
    """addChatMessage, then wait for the chat observer to report it back.

    This is the one round trip that stays entirely inside the client: the text
    is turned into a ChatComponentText, handed to EntityPlayerSP.addChatMessage,
    rendered into the chat box -- and on the way through GuiNewChat
    .printChatMessage the hook sees it and pushes it back out of the socket it
    arrived on.  Both halves of the chat feature in one exchange.
    """
    import websockets

    async with websockets.connect(f"ws://127.0.0.1:{port}", open_timeout=20) as ws:
        await ws.send(json.dumps({
            "cmd": "net.minecraft.client.entity.EntityPlayerSP.addChatMessage",
            "text": text}))
        # The EVENT ARRIVES FIRST.  The detour on printChatMessage runs inside
        # the addChatMessage call, on this very request's thread, so the line is
        # broadcast before the reply to the command that caused it is written.
        # An earlier version of this waited for the reply and only then looked
        # for the echo, and therefore always missed it -- the harness reported a
        # broken bridge for a bridge that was working.
        deadline = time.time() + 25
        seen = False
        while time.time() < deadline:
            try:
                frame = json.loads(await asyncio.wait_for(
                    ws.recv(), timeout=max(0.5, deadline - time.time())))
            except asyncio.TimeoutError:
                break
            if "ok" in frame:
                if not frame.get("ok"):
                    return False
                if seen:
                    return True
            elif text in json.dumps(frame):
                seen = True
    return seen


def wait_for_feature(port: int, feature: str, timeout: float = 90.0) -> bool:
    """Wait until chatwire reports `feature` as started.

    A feature whose class was not loaded at inject time comes up later, and
    until it does the thing it provides does not happen.  Asserting on the chat
    echo before the chat observer exists tests the harness's patience, not
    chatwire -- which is what it did, intermittently, on whichever client
    happened to be injected earliest.
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            reply = ask(port, [{"cmd": "system.status"}])[0]["reply"]
            for f in reply.get("result", {}).get("features", []):
                if f.get("name") == feature and f.get("started"):
                    return True
        except Exception:                                      # noqa: BLE001
            pass
        time.sleep(2)
    return False


def add_chat_echo(mapping: str, port: int) -> int:
    if not wait_for_feature(port, "chat"):
        say(f"    {'addChatMessage':<18} the chat feature never started")
        return 1
    text = f"chatwire {mapping} local echo"
    try:
        heard = asyncio.run(_add_chat_echo(port, text))
    except Exception as e:                                     # noqa: BLE001
        say(f"    {'addChatMessage':<18} FAILED: {e}")
        return 1
    say(f"    {'addChatMessage':<18} shown and observed back: {heard}")
    return 0 if heard else 1


def chat_round_trip(mapping: str, port: int, server: subprocess.Popen | None) -> int:
    """Say something as this client and confirm the SERVER heard it.

    The strongest end-to-end check the harness has: the text starts as JSON on a
    socket, becomes a Java call inside the game, becomes a packet, and comes back
    out in another process's log.  Nothing about that can pass by accident.
    """
    if server is None:
        return 0
    line = f"chatwire {mapping} round trip"
    exchanges = ask(port, [{"cmd": "net.minecraft.client.entity.EntityPlayerSP.sendChatMessage",
                            "text": line}])
    _summarise(mapping, exchanges)
    if not exchanges[0]["reply"].get("ok"):
        return 1

    log = server_mod.log_path()
    deadline = time.time() + 20
    while time.time() < deadline:
        if log.is_file() and line in log.read_text(encoding="utf-8", errors="replace"):
            say(f"    {'server heard it':<18} yes")
            return 0
        time.sleep(1)
    say(f"    {'server heard it':<18} NO -- the message never reached the server")
    return 1


def run(which: tuple[str, ...], keep: bool = False, timeout: float = 180.0,
        with_server: bool = True) -> int:
    """Bring every client up, inject a chatwire into each, then ask them all.

    They run AT THE SAME TIME, one chatwire per client, each on its own port.
    That is the arrangement worth testing rather than a convenience: three
    bridges into three JVMs is what a user driving several accounts has, and
    nothing about it works by accident -- each chatwire is a separate DLL in a
    separate process with its own listening socket, and the only thing they
    share is the machine's port space.  Running them one at a time would never
    have exercised that.
    """
    clients: dict[str, subprocess.Popen] = {}
    server: subprocess.Popen | None = None
    failures = 0
    try:
        usernames = {m: f"chatwire_{m[:3]}" for m in which}
        if with_server:
            server = server_mod.start(ops=tuple(usernames.values()))
            if server is None:
                say("  continuing without a server; the in-world checks will be skipped")

        join = f"127.0.0.1:{server_mod.DEFAULT_PORT}" if server else None
        for mapping in which:
            say(f"* {mapping} -- {paths.MAPPING_DESC[mapping]}")
            clients[mapping] = launch_mod.launch(
                mapping, wait=False, username=usernames[mapping], join=join)

        up: list[str] = []
        for mapping, proc in clients.items():
            if wait_for_menu(mapping, proc, timeout):
                up.append(mapping)
            else:
                say(f"    {mapping}: never reached the main menu; see "
                    f"{paths.mapping_run(mapping) / 'launch.log'}")
                failures += 1

        say("")
        bridged = [m for m in up if inject(m, clients[m], PORTS[m])]
        failures += len(up) - len(bridged)

        # Every client is now running with its own chatwire.  The queries below
        # go out over three sockets that are all open at once.
        listening = [m for m in bridged if wait_for_port(PORTS[m])]
        failures += len(bridged) - len(listening)
        for m in bridged:
            if m not in listening:
                say(f"    {m}: chatwire never bound port {PORTS[m]}")
        if listening:
            say("")
            say(f"* {len(listening)} chatwire(s) live at once on ports "
                + ", ".join(str(PORTS[m]) for m in listening))

        for mapping in listening:
            say("")
            say(f"* {mapping} on port {PORTS[mapping]}")
            try:
                failures += 1 if _summarise(mapping, ask(PORTS[mapping], QUERIES)) else 0
                if server is not None:
                    if wait_in_world(PORTS[mapping]):
                        _summarise(mapping, ask(PORTS[mapping], IN_WORLD_QUERIES))
                        failures += chat_round_trip(mapping, PORTS[mapping], server)
                        failures += add_chat_echo(mapping, PORTS[mapping])
                    else:
                        say("    never joined the server; in-world checks skipped")
                        failures += 1
            except Exception as e:                             # noqa: BLE001
                say(f"    could not talk to it: {e}")
                failures += 1

        if not keep:
            for mapping in listening:
                try:
                    ask(PORTS[mapping], [{"cmd": "system.detach"}])
                except Exception:                              # noqa: BLE001
                    pass
    finally:
        if not keep:
            time.sleep(2)
            for proc in clients.values():
                stop(proc)
            # The server last, and asked rather than killed: it has a world to
            # save, and the next run reuses it.
            server_mod.stop(server)

    say("")
    if failures:
        say(f"* {failures} problem(s) across {len(which)} client(s)")
    else:
        say(f"* chatwire's name table is complete under all {len(which)} mappings, "
            f"with {len(which)} bridges live at once")
    return 1 if failures else 0
