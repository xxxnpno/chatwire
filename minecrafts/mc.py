#!/usr/bin/env python3
"""Minecraft 1.8.9, in all three of its mappings.

    python mc.py setup                 download Mojang's files and the mappings
    python mc.py build [mapping...]    produce the vanilla / srg / mcp jars
    python mc.py decompile [mapping]   Vineflower each jar into <mapping>/src
    python mc.py launch <mapping>      start it
    python mc.py launch --all          start all three, wait for each main menu
    python mc.py status                what exists on disk right now
    python mc.py all                   setup + build + status

`launch` takes `--detach` to return as soon as the process is up, `--timeout N`
to stop it after N seconds, and `--` to pass the rest through to the game.
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from mcbuild import bridge as bridge_mod                    # noqa: E402
from mcbuild import build as build_mod                      # noqa: E402
from mcbuild import check as check_mod                      # noqa: E402
from mcbuild import decompile as decompile_mod              # noqa: E402
from mcbuild import launch as launch_mod                    # noqa: E402
from mcbuild import mappings, paths, vanilla                # noqa: E402
from mcbuild import server as server_mod                    # noqa: E402
from mcbuild.util import human, say                         # noqa: E402


def _mappings_arg(values: list[str] | None) -> tuple[str, ...]:
    if not values or "all" in values:
        return paths.MAPPINGS_ORDER
    for v in values:
        if v not in paths.MAPPINGS_ORDER:
            raise SystemExit(f"unknown mapping {v!r}; pick from {', '.join(paths.MAPPINGS_ORDER)}")
    return tuple(values)


def cmd_setup(_args: argparse.Namespace) -> int:
    vanilla.setup()
    say("* mappings")
    mappings.setup()
    say("* ready -- next: python mc.py build")
    return 0


def cmd_build(args: argparse.Namespace) -> int:
    which = _mappings_arg(args.mapping)
    say("* building " + ", ".join(which))
    jars = build_mod.build(which)
    for m in which:
        if m in jars:
            launch_mod.write_script(m)
    say("* launch scripts written next to mc.py")
    return 0


def cmd_decompile(args: argparse.Namespace) -> int:
    for m in _mappings_arg(args.mapping):
        decompile_mod.decompile(m, force=args.force)
    return 0


def cmd_launch(args: argparse.Namespace) -> int:
    which = _mappings_arg(args.mapping if args.mapping else None) if args.all \
        else _mappings_arg(args.mapping)
    if not args.all and len(which) != 1:
        raise SystemExit("launch takes one mapping, or --all")

    extra_jvm = args.jvm or []
    extra_game = args.game or []

    if args.all:
        started = []
        for m in which:
            proc = launch_mod.launch(m, wait=False, username=f"{args.username}_{m[:3]}",
                                     memory=args.memory, extra_jvm=extra_jvm,
                                     extra_game=extra_game)
            started.append((m, proc.pid))
            time.sleep(2)
        say(f"* started {len(started)}: " + ", ".join(f"{m}={p}" for m, p in started))
        return 0

    return launch_mod.launch(which[0], wait=not args.detach, timeout=args.timeout,
                             username=args.username, memory=args.memory,
                             extra_jvm=extra_jvm, extra_game=extra_game)


def cmd_chatwire(args: argparse.Namespace) -> int:
    return bridge_mod.run(_mappings_arg(args.mapping), keep=args.keep,
                          timeout=args.timeout, with_server=not args.no_server)


def cmd_server(args: argparse.Namespace) -> int:
    proc = server_mod.start(ops=tuple(args.op or ()))
    if proc is None:
        return 1
    say("* running; Ctrl-C to stop")
    try:
        proc.wait()
    except KeyboardInterrupt:
        server_mod.stop(proc)
    return 0


def cmd_check(args: argparse.Namespace) -> int:
    return check_mod.check(args.header)


def cmd_status(_args: argparse.Namespace) -> int:
    say(f"minecrafts/ for Minecraft {paths.VERSION}   ({paths.ROOT})")
    say("")

    def line(label: str, path: Path, detail: str = "") -> None:
        if path.is_dir():
            n = sum(1 for _ in path.rglob("*") if _.is_file())
            say(f"  {label:<24} {'yes' if n else 'empty':<6} {n} files {detail}")
        elif path.is_file():
            say(f"  {label:<24} {'yes':<6} {human(path.stat().st_size)} {detail}")
        else:
            say(f"  {label:<24} {'no':<6} {detail}")

    line("client jar", paths.SHARED / f"{paths.VERSION}-client.jar")
    line("libraries", paths.LIBRARIES)
    line("natives", paths.NATIVES)
    line("assets", paths.ASSETS)
    line("joined.srg", paths.MAPPINGS / "joined.srg")
    line("srg-to-mcp.srg", paths.MAPPINGS / "srg-to-mcp.srg")
    line("unified mappings", paths.MAPPINGS / f"{paths.VERSION}-mappings.json")
    say("")
    for m in paths.MAPPINGS_ORDER:
        line(f"{m} jar", paths.mapping_jar(m), f"-- {paths.MAPPING_DESC[m]}")
        line(f"{m} src", paths.mapping_src(m))
    say("")
    try:
        say(f"  java (game)  {paths.java_game()}")
    except SystemExit as e:
        say(f"  java (game)  MISSING: {e}")
    say(f"  java (tools) {paths.java_tools()}")
    return 0


def cmd_all(args: argparse.Namespace) -> int:
    cmd_setup(args)
    cmd_build(args)
    return cmd_status(args)


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="mc.py", description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    sp = sub.add_parser("setup", help="download Mojang's files and the mappings")
    sp.set_defaults(func=cmd_setup)

    sp = sub.add_parser("build", help="produce the vanilla / srg / mcp jars")
    sp.add_argument("mapping", nargs="*", help="default: all three")
    sp.set_defaults(func=cmd_build)

    sp = sub.add_parser("decompile", help="Vineflower each jar into <mapping>/src")
    sp.add_argument("mapping", nargs="*", help="default: all three")
    sp.add_argument("--force", action="store_true")
    sp.set_defaults(func=cmd_decompile)

    sp = sub.add_parser("launch", help="start a client")
    sp.add_argument("mapping", nargs="*")
    sp.add_argument("--all", action="store_true", help="start all three, detached")
    sp.add_argument("--detach", action="store_true")
    sp.add_argument("--timeout", type=float, default=None, help="stop after N seconds")
    sp.add_argument("--username", default=launch_mod.DEFAULT_USERNAME)
    sp.add_argument("--memory", default="2G")
    sp.add_argument("--jvm", action="append", help="extra JVM argument (repeatable)")
    sp.add_argument("--game", action="append", help="extra game argument (repeatable)")
    sp.set_defaults(func=cmd_launch)

    sp = sub.add_parser("chatwire", help="inject chatwire into each client and ask it")
    sp.add_argument("mapping", nargs="*", help="default: all three")
    sp.add_argument("--keep", action="store_true", help="leave the clients running")
    sp.add_argument("--timeout", type=float, default=180.0)
    sp.add_argument("--no-server", action="store_true",
                    help="do not start a server; skip the in-world checks")
    sp.set_defaults(func=cmd_chatwire)

    sp = sub.add_parser("server", help="run a local 1.8.9 server for the clients to join")
    sp.add_argument("--op", action="append", help="username to op (repeatable)")
    sp.set_defaults(func=cmd_server)

    sp = sub.add_parser("check", help="check chatwire's name table against the jar")
    sp.add_argument("--header", type=Path, default=None)
    sp.set_defaults(func=cmd_check)

    sp = sub.add_parser("status", help="what exists on disk right now")
    sp.set_defaults(func=cmd_status)

    sp = sub.add_parser("all", help="setup + build + status")
    sp.add_argument("mapping", nargs="*")
    sp.set_defaults(func=cmd_all)

    args = p.parse_args(argv)
    return args.func(args) or 0


if __name__ == "__main__":
    raise SystemExit(main())
