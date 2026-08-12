"""Decompile each jar to readable Java with Vineflower.

Vineflower writes a sources *archive* when handed a jar, so we decompile to a
zip and unpack that.  Unpacking is where the obfuscated mapping gets
interesting: `a.class` and `A.class` are different classes to the JVM and the
same file to Windows.  Colliding names get a `#` suffix rather than silently
overwriting each other.
"""

from __future__ import annotations

import shutil
import zipfile
from pathlib import Path

from . import paths, tools, vanilla
from .util import say

#: -dgs generic signatures, -rsy synthetic members hidden, -asc ascii escapes,
#: -jrt resolve java.* from this JVM, -log WARN to keep the output readable.
VINEFLOWER_OPTS = ["-dgs=1", "-rsy=1", "-asc=1", "-jrt=1", "-log=WARN", "-thr=8"]


def _unpack_sources(archive: Path, dest: Path) -> tuple[int, int]:
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True, exist_ok=True)

    written: dict[str, str] = {}                   # lowercased path -> real path
    files = collisions = 0
    with zipfile.ZipFile(archive) as z:
        for name in z.namelist():
            if name.endswith("/"):
                continue
            out_name = name
            n = 0
            while out_name.lower() in written:
                n += 1
                stem, _, ext = name.rpartition(".")
                out_name = f"{stem}#{n}.{ext}" if ext else f"{name}#{n}"
            if n:
                collisions += 1
            written[out_name.lower()] = out_name
            target = dest / out_name
            target.parent.mkdir(parents=True, exist_ok=True)
            with z.open(name) as src, target.open("wb") as out:
                shutil.copyfileobj(src, out)
            files += 1
    return files, collisions


def decompile(mapping: str, force: bool = False) -> Path:
    jar = paths.mapping_jar(mapping)
    if not jar.is_file():
        raise SystemExit(f"{jar} is missing -- run `python mc.py build` first")

    src = paths.mapping_src(mapping)
    stamp = src / ".jar-sha"
    from .util import run, sha1_of

    digest = sha1_of(jar)
    if not force and stamp.is_file() and stamp.read_text().strip() == digest:
        say(f"  {mapping}: sources up to date")
        return src

    work = paths.mapping_dir(mapping) / ".decompile"
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)

    vj = vanilla.version_json()
    libs = [f"-e={p}" for p in vanilla.classpath(vj) if p.is_file()]

    say(f"  {mapping}: decompiling {jar.name} (this takes a few minutes)")
    run([
        str(paths.java_tools()), "-Xmx4G", "-jar", str(tools.vineflower()),
        *VINEFLOWER_OPTS, *libs, str(jar), str(work),
    ])

    produced = next((p for p in work.iterdir() if p.suffix in (".jar", ".zip")), None)
    if produced is None:
        raise RuntimeError(f"Vineflower produced nothing in {work}")

    files, collisions = _unpack_sources(produced, src)
    shutil.rmtree(work, ignore_errors=True)
    stamp.write_text(digest, encoding="utf-8")
    note = f", {collisions} case collisions renamed" if collisions else ""
    say(f"  {mapping}: {files} source files{note} -> {src}")
    return src
