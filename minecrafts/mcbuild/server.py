"""A local 1.8.9 server, so the clients have a world to be in.

Most of what chatwire reads does not exist at the main menu: `thePlayer` is
null, there is no world, and an inventory is a question with no answer.  Testing
the API there proves only that it says "not in a world" politely.

So the harness runs Mojang's own 1.8.9 server -- offline mode, flat world,
creative, all three test accounts opped -- and the clients are started with
`--server 127.0.0.1`, which 1.8.9's `Main` accepts and which makes them join on
launch with no GUI to drive.  Opped matters: it is what lets chatwire drive the
game through `sendChatMessage("/give ...")` and then read the result back
through the same socket, which is a whole test with no human in it.
"""

from __future__ import annotations

import json
import subprocess
import time
import uuid as uuidlib
from pathlib import Path

from . import paths
from .launch import offline_uuid
from .util import fetch, human, say

DEFAULT_PORT = 25565

#: Small, flat, empty and instant.  Every setting here is about making the world
#: the same every run rather than about making it pleasant to play in.
PROPERTIES = {
    "online-mode": "false",
    "level-type": "FLAT",
    "level-name": "chatwire",
    "level-seed": "1189",
    "generate-structures": "false",
    "spawn-npcs": "false",
    "spawn-animals": "false",
    "spawn-monsters": "false",
    "gamemode": "1",
    "difficulty": "0",
    "force-gamemode": "true",
    "max-players": "10",
    "view-distance": "4",
    "spawn-protection": "0",
    "allow-nether": "false",
    "enable-command-block": "false",
    "announce-player-achievements": "false",
    "motd": "chatwire test server",
}

#: What the server prints once it is accepting connections.
READY_MARKER = 'Done ('


def directory() -> Path:
    return paths.SHARED / "server"


def log_path() -> Path:
    return directory() / "server.log"


def jar() -> Path:
    from . import vanilla

    vj = vanilla.version_json()
    d = vj["downloads"]["server"]
    dest = paths.SHARED / f"{paths.VERSION}-server.jar"
    if not dest.is_file():
        say(f"  server jar: {human(d['size'])}")
    return fetch(d["url"], dest, d["sha1"], d["size"])


def prepare(port: int = DEFAULT_PORT, ops: tuple[str, ...] = ()) -> Path:
    run = directory()
    run.mkdir(parents=True, exist_ok=True)

    # Mojang's EULA, accepted here because this is Mojang's server running
    # Mojang's game on the machine of someone who already owns it.
    (run / "eula.txt").write_text("eula=true\n", encoding="utf-8")

    props = dict(PROPERTIES, **{"server-port": str(port)})
    (run / "server.properties").write_text(
        "\n".join(f"{k}={v}" for k, v in sorted(props.items())) + "\n", encoding="utf-8")

    # Offline UUIDs, derived exactly as the server derives them, so an opped
    # entry actually matches the player who joins.  A name alone would not: the
    # server keys ops by UUID.
    (run / "ops.json").write_text(
        json.dumps([{"uuid": offline_uuid(name), "name": name,
                     "level": 4, "bypassesPlayerLimit": True} for name in ops], indent=2),
        encoding="utf-8")
    return run


def start(port: int = DEFAULT_PORT, ops: tuple[str, ...] = (),
          memory: str = "1G", timeout: float = 180.0) -> subprocess.Popen | None:
    """Start the server and wait until it accepts connections."""
    run = prepare(port, ops)
    server_jar = jar()
    log = log_path()

    say(f"* server on 127.0.0.1:{port} ({run})")
    with log.open("w", encoding="utf-8", errors="replace") as sink:
        proc = subprocess.Popen(
            [str(paths.java_game()), f"-Xmx{memory}", "-jar", str(server_jar), "nogui"],
            cwd=run, stdin=subprocess.PIPE, stdout=sink, stderr=subprocess.STDOUT, text=True)

    deadline = time.time() + timeout
    while time.time() < deadline:
        if proc.poll() is not None:
            say(f"  the server exited ({proc.returncode}); see {log}")
            return None
        if log.is_file() and READY_MARKER in log.read_text(encoding="utf-8", errors="replace"):
            say(f"  ready, {len(ops)} operator(s)")
            return proc
        time.sleep(0.5)

    say(f"  the server never finished starting; see {log}")
    stop(proc)
    return None


def stop(proc: subprocess.Popen | None) -> None:
    """Ask the server to save and shut down; kill it only if it will not."""
    if proc is None or proc.poll() is not None:
        return
    try:
        if proc.stdin:
            proc.stdin.write("stop\n")
            proc.stdin.flush()
        proc.wait(timeout=30)
    except Exception:                                          # noqa: BLE001
        try:
            proc.terminate()
        except OSError:
            pass


def console(proc: subprocess.Popen | None, line: str) -> bool:
    """Type one line at the server console.  `say hello`, `op someone`, ..."""
    if proc is None or proc.poll() is not None or not proc.stdin:
        return False
    try:
        proc.stdin.write(line.rstrip("\n") + "\n")
        proc.stdin.flush()
        return True
    except Exception:                                          # noqa: BLE001
        return False


def uuid_of(name: str) -> str:
    return str(uuidlib.UUID(offline_uuid(name).replace("-", "")))
