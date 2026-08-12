"""Where everything lives, and which JVM runs what.

The tree is deliberately self-contained: `minecrafts/` can be deleted and rebuilt
from the network alone.  Nothing outside it is written to.  `%APPDATA%/.minecraft`
is read, never modified -- it is only a download cache we can seed from.
"""

from __future__ import annotations

import os
import shutil
from pathlib import Path

VERSION = "1.8.9"

# minecrafts/mcbuild/paths.py -> minecrafts/
ROOT = Path(__file__).resolve().parent.parent

SHARED = ROOT / "shared"
TOOLS = SHARED / "tools"
LIBRARIES = SHARED / "libraries"
NATIVES = SHARED / "natives"
ASSETS = SHARED / "assets"
MAPPINGS = SHARED / "mappings"
META = SHARED / "meta"

#: The three mappings, in the order a name travels through them.
MAPPINGS_ORDER = ("vanilla", "srg", "mcp")

#: What each one actually is, for `mc.py status` and the README.
MAPPING_DESC = {
    "vanilla": "obfuscated (notch) names, straight from Mojang",
    "srg": "searge names -- func_71407_l, field_71425_J, stable across 1.8.x",
    "mcp": "MCP names -- runTick, running; what the community reads",
}


def mapping_dir(mapping: str) -> Path:
    return ROOT / mapping


def mapping_jar(mapping: str) -> Path:
    return mapping_dir(mapping) / f"{VERSION}-{mapping}.jar"


def mapping_src(mapping: str) -> Path:
    return mapping_dir(mapping) / "src"


def mapping_run(mapping: str) -> Path:
    return mapping_dir(mapping) / "run"


def dot_minecraft() -> Path | None:
    """The launcher's install, if there is one.  Read-only, used as a cache."""
    appdata = os.environ.get("APPDATA")
    if not appdata:
        return None
    p = Path(appdata) / ".minecraft"
    return p if p.is_dir() else None


def _java_home_candidates() -> list[Path]:
    roots = [
        Path(r"C:\Program Files\Eclipse Adoptium"),
        Path(r"C:\Program Files\Java"),
        Path(r"C:\Program Files\Microsoft\jdk"),
        Path(r"C:\Program Files\Amazon Corretto"),
        Path(r"C:\Program Files\Zulu"),
    ]
    out: list[Path] = []
    for r in roots:
        if r.is_dir():
            out.extend(sorted(p for p in r.iterdir() if (p / "bin" / "java.exe").exists()))
    return out


def _major_of(java_exe: Path) -> int | None:
    """Read the JVM's major version out of the `release` file -- no subprocess."""
    release = java_exe.parent.parent / "release"
    if not release.is_file():
        return None
    for line in release.read_text(errors="replace").splitlines():
        if line.startswith("JAVA_VERSION="):
            v = line.split("=", 1)[1].strip().strip('"')
            parts = v.split(".")
            if parts[0] == "1" and len(parts) > 1:
                return int(parts[1])            # 1.8.0_492 -> 8
            return int("".join(c for c in parts[0] if c.isdigit()))
    return None


def java_for(major: int) -> Path:
    """A `java.exe` of exactly `major`.  1.8.9 will not start on anything else."""
    env = os.environ.get(f"MC_JAVA{major}")
    if env:
        return Path(env)
    for home in _java_home_candidates():
        exe = home / "bin" / "java.exe"
        if _major_of(exe) == major:
            return exe
    raise SystemExit(
        f"No Java {major} found.  Install one, or set MC_JAVA{major} to its java.exe.\n"
        f"Looked under: " + ", ".join(str(p) for p in _java_home_candidates()) or "(nothing)"
    )


def java_game() -> Path:
    """Minecraft 1.8.9 needs Java 8: LWJGL 2.9.4 and the class files predate 9."""
    return java_for(8)


def java_tools() -> Path:
    """SpecialSource and Vineflower: anything modern, else fall back to the game JVM."""
    for major in (21, 17, 26, 25, 24, 23, 22, 11):
        try:
            return java_for(major)
        except SystemExit:
            continue
    which = shutil.which("java")
    if which:
        return Path(which)
    return java_game()


def ensure_tree() -> None:
    for d in (SHARED, TOOLS, LIBRARIES, NATIVES, ASSETS, MAPPINGS, META):
        d.mkdir(parents=True, exist_ok=True)
    for m in MAPPINGS_ORDER:
        mapping_dir(m).mkdir(parents=True, exist_ok=True)
