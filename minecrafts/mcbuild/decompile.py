"""Decompile each jar to readable Java with Vineflower.

Handed a jar and a directory, Vineflower writes the sources loose into it and
copies the non-class entries across, so `<mapping>/src` is the decompiled game
plus its assets.

The obfuscated jar is the one that could have gone wrong here: `a.class` and
`A.class` are two classes to the JVM and one filename to Windows.  1.8.9's
obfuscator happens to emit lowercase names only -- checked, not assumed, by
`case_collisions()` below, which refuses the decompile rather than letting the
filesystem silently drop half a pair.
"""

from __future__ import annotations

import shutil
import zipfile
from collections import Counter
from pathlib import Path

from . import paths, tools, vanilla
from .util import run, say, sha1_of

#: -dgs generic signatures, -rsy synthetic members hidden, -asc ascii escapes,
#: -jrt resolve java.* against this JVM, -log WARN to keep the output readable.
VINEFLOWER_OPTS = ["-dgs=1", "-rsy=1", "-asc=1", "-jrt=1", "-log=WARN", "-thr=8"]


def case_collisions(jar: Path) -> list[list[str]]:
    """Entries that differ only in case, and would therefore share a filename."""
    with zipfile.ZipFile(jar) as z:
        names = [n for n in z.namelist() if not n.endswith("/")]
    seen = Counter(n.lower() for n in names)
    return [[n for n in names if n.lower() == k] for k, v in seen.items() if v > 1]


def decompile(mapping: str, force: bool = False) -> Path:
    jar = paths.mapping_jar(mapping)
    if not jar.is_file():
        raise SystemExit(f"{jar} is missing -- run `python mc.py build` first")

    src = paths.mapping_src(mapping)
    stamp = src / ".jar-sha"
    digest = sha1_of(jar)
    if not force and stamp.is_file() and stamp.read_text().strip() == digest:
        say(f"  {mapping}: sources up to date")
        return src

    clashes = case_collisions(jar)
    if clashes:
        for pair in clashes[:5]:
            say(f"  ! {' and '.join(pair)} differ only in case")
        raise SystemExit(
            f"{jar.name} has {len(clashes)} case-colliding entries; a Windows "
            f"filesystem would silently keep one of each pair"
        )

    if src.exists():
        shutil.rmtree(src)
    src.mkdir(parents=True)

    vj = vanilla.version_json()
    libs = [f"-e={p}" for p in vanilla.classpath(vj) if p.is_file()]

    say(f"  {mapping}: decompiling {jar.name} (a few minutes)")
    run([
        str(paths.java_tools()), "-Xmx4G", "-jar", str(tools.vineflower()),
        *VINEFLOWER_OPTS, *libs, str(jar), str(src),
    ], quiet=True)

    java = sum(1 for _ in src.rglob("*.java"))
    if not java:
        raise RuntimeError(f"Vineflower wrote no .java into {src}")
    stamp.write_text(digest, encoding="utf-8")
    say(f"  {mapping}: {java} source files -> {src}")
    return src
