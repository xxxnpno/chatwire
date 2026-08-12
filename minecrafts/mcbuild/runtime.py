"""Mojang's own Java runtime for this version, downloaded like everything else.

1.8.9's version manifest names the runtime it wants -- `jre-legacy`, which is
Java 8u51 -- and the launcher installs exactly that.  `minecrafts/` did not: it
picked whichever Java 8 happened to be on the machine, which for this one was
Adoptium 8u492, eleven years newer.

That is not a detail.  A JVM is not just something that runs the game; it is the
thing chatwire reads.  vmhook resolves HotSpot's internals through the
`gHotSpotVMStructs` table the JVM exports, and which fields are in that table
differs between builds:

    Adoptium 8u492   ClassLoaderData::_klasses absent (as on all JDK 8), so
                     vmhook falls back to SystemDictionary -- and on that build
                     the fallback resolves nothing, not even java/lang/String.
    Adoptium 21      _klasses present, the modern walk works.

Testing chatwire against a JVM no 1.8.9 player runs proves nothing about the
ones they do.  So the runtime comes from Mojang, pinned by the version manifest,
like the client jar and the assets.
"""

from __future__ import annotations

import json
import platform
import shutil
from pathlib import Path

from . import paths
from .util import fetch, fetch_many, human, say

ALL_RUNTIMES_URL = (
    "https://launchermeta.mojang.com/v1/products/java-runtime/"
    "2ec0cc96c44e5a76b9c8b7c39df7210883d12871/all.json"
)

#: What 1.8.9 asks for when its version json has no `javaVersion` block.
DEFAULT_COMPONENT = "jre-legacy"


def platform_key() -> str:
    machine = platform.machine().lower()
    if machine in ("arm64", "aarch64"):
        return "windows-arm64"
    if machine in ("x86", "i386", "i686"):
        return "windows-x86"
    return "windows-x64"


def component_for(vj: dict) -> str:
    return (vj.get("javaVersion") or {}).get("component") or DEFAULT_COMPONENT


def home(component: str) -> Path:
    return paths.SHARED / "runtime" / component


def java_exe(component: str = DEFAULT_COMPONENT) -> Path:
    """Where this runtime's launcher lives once installed.

    jre-legacy unpacks as a JRE (`bin/java.exe`); the modern components unpack
    with the same layout, so one path serves both.
    """
    return home(component) / "bin" / "java.exe"


def _manifest_url(component: str) -> tuple[str, str]:
    doc = json.loads(
        fetch(ALL_RUNTIMES_URL, paths.META / "java-runtime-all.json").read_text("utf-8"))
    key = platform_key()
    entries = (doc.get(key) or {}).get(component) or []
    if not entries:
        raise SystemExit(
            f"Mojang publishes no {component} for {key}.  Set MC_JAVA8 to a "
            f"Java 8 java.exe and it will be used instead.")
    return entries[0]["manifest"]["url"], entries[0]["version"]["name"]


def install(vj: dict | None = None, component: str | None = None) -> Path:
    """Download the runtime and return its java.exe.  Idempotent."""
    from . import vanilla

    vj = vj or vanilla.version_json()
    component = component or component_for(vj)
    exe = java_exe(component)
    stamp = home(component) / ".installed"
    if exe.is_file() and stamp.is_file():
        return exe

    url, version = _manifest_url(component)
    manifest = json.loads(
        fetch(url, paths.META / f"{component}.json").read_text("utf-8"))
    files: dict = manifest["files"]

    root = home(component)
    jobs = []
    links: list[tuple[Path, str]] = []
    total = 0
    for rel, info in files.items():
        target = root / rel
        kind = info.get("type")
        if kind == "directory":
            target.mkdir(parents=True, exist_ok=True)
        elif kind == "link":
            links.append((target, info["target"]))
        elif kind == "file":
            raw = info["downloads"]["raw"]
            jobs.append((raw["url"], target, raw.get("sha1"), raw.get("size"), None))
            total += raw.get("size", 0)

    say(f"  runtime: {component} {version}, {len(jobs)} files, {human(total)}")
    fetch_many(jobs, "runtime", workers=24)

    # Windows has symlinks but creating one needs a privilege a normal user does
    # not have.  A JRE's links are duplicates of small files, so copying is both
    # correct and cheaper than asking for elevation.
    for target, rel_target in links:
        source = (target.parent / rel_target).resolve()
        if source.is_file() and not target.exists():
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, target)

    if not exe.is_file():
        raise RuntimeError(f"{component} unpacked but {exe} is missing")
    stamp.write_text(version, encoding="utf-8")
    say(f"  runtime: {exe}")
    return exe
