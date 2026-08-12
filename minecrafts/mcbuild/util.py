"""Downloading, hashing, unzipping, and running a subprocess -- with progress."""

from __future__ import annotations

import hashlib
import json
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request
import zipfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

USER_AGENT = "chatwire-minecrafts/1.0 (+https://github.com/xxxnpno/chatwire)"
_TIMEOUT = 60


def say(msg: str) -> None:
    print(msg, flush=True)


def sha1_of(path: Path) -> str:
    h = hashlib.sha1()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def ok(path: Path, sha1: str | None, size: int | None = None) -> bool:
    """Is `path` already the file we wanted?"""
    if not path.is_file():
        return False
    if size is not None and path.stat().st_size != size:
        return False
    if sha1 is not None:
        return sha1_of(path) == sha1.lower()
    return path.stat().st_size > 0


def _get(url: str) -> bytes:
    last: Exception | None = None
    for attempt in range(4):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
            with urllib.request.urlopen(req, timeout=_TIMEOUT) as r:
                return r.read()
        except (urllib.error.URLError, TimeoutError, ConnectionError) as e:
            last = e
            time.sleep(1.5 * (attempt + 1))
    raise RuntimeError(f"download failed: {url}: {last}")


def fetch(url: str, dest: Path, sha1: str | None = None, size: int | None = None,
          seed: Path | None = None) -> Path:
    """Put `url` at `dest`.  Skip if already correct; copy from `seed` if that is."""
    if ok(dest, sha1, size):
        return dest
    dest.parent.mkdir(parents=True, exist_ok=True)
    if seed is not None and ok(seed, sha1, size):
        shutil.copyfile(seed, dest)
        return dest
    data = _get(url)
    if sha1 is not None and hashlib.sha1(data).hexdigest() != sha1.lower():
        raise RuntimeError(f"sha1 mismatch for {url}")
    tmp = dest.with_suffix(dest.suffix + ".part")
    tmp.write_bytes(data)
    tmp.replace(dest)
    return dest


def fetch_json(url: str, dest: Path, sha1: str | None = None) -> dict:
    fetch(url, dest, sha1)
    return json.loads(dest.read_text(encoding="utf-8"))


def fetch_many(jobs: list[tuple[str, Path, str | None, int | None, Path | None]],
               label: str, workers: int = 16) -> None:
    """Fetch in parallel.  `jobs` are (url, dest, sha1, size, seed) tuples."""
    todo = [j for j in jobs if not ok(j[1], j[2], j[3])]
    if not todo:
        say(f"  {label}: {len(jobs)} already present")
        return
    done = 0
    errors: list[str] = []

    def one(job):
        nonlocal done
        url, dest, sha1, size, seed = job
        try:
            fetch(url, dest, sha1, size, seed)
        except Exception as e:                                  # noqa: BLE001
            errors.append(f"{url}: {e}")
        done += 1
        if done % 25 == 0 or done == len(todo):
            print(f"\r  {label}: {done}/{len(todo)}", end="", flush=True)

    with ThreadPoolExecutor(max_workers=workers) as pool:
        list(pool.map(one, todo))
    print(flush=True)
    if errors:
        for e in errors[:10]:
            say(f"  ! {e}")
        raise RuntimeError(f"{len(errors)} of {len(todo)} {label} failed")


def unzip(archive: Path, dest: Path, members: list[str] | None = None,
          exclude: list[str] | None = None) -> None:
    dest.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(archive) as z:
        names = members if members is not None else z.namelist()
        for name in names:
            if name.endswith("/"):
                continue
            if exclude and any(name.startswith(p) for p in exclude):
                continue
            target = dest / name
            target.parent.mkdir(parents=True, exist_ok=True)
            with z.open(name) as src, target.open("wb") as out:
                shutil.copyfileobj(src, out)


def read_in_zip(archive: Path, name: str) -> bytes:
    with zipfile.ZipFile(archive) as z:
        return z.read(name)


def find_in_zip(archive: Path, basename: str) -> str | None:
    with zipfile.ZipFile(archive) as z:
        for n in z.namelist():
            if n.rsplit("/", 1)[-1] == basename:
                return n
    return None


def run(cmd: list[str], cwd: Path | None = None, quiet: bool = False) -> int:
    if not quiet:
        say("  $ " + " ".join(f'"{c}"' if " " in c else c for c in cmd))
    proc = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True, errors="replace")
    if proc.returncode != 0:
        sys.stdout.write(proc.stdout or "")
        raise RuntimeError(f"command failed ({proc.returncode}): {cmd[0]}")
    return proc.returncode


def human(n: int) -> str:
    x = float(n)
    for unit in ("B", "KiB", "MiB", "GiB"):
        if x < 1024 or unit == "GiB":
            return f"{x:.0f} {unit}" if unit == "B" else f"{x:.1f} {unit}"
        x /= 1024.0
    return f"{n} B"
