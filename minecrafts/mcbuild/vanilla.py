"""Everything Mojang ships: the version manifest, the client jar, the libraries,
the Windows natives, and the asset objects.

The launcher's own install under `%APPDATA%/.minecraft` is used as a download
cache -- every candidate is sha1-checked before it is copied, so a corrupt or
foreign file falls through to the network instead of poisoning the tree.
"""

from __future__ import annotations

import json
from pathlib import Path

from . import paths
from .util import fetch, fetch_json, fetch_many, human, say, unzip

MANIFEST_URL = "https://launchermeta.mojang.com/mc/game/version_manifest_v2.json"


# --------------------------------------------------------------------------- meta


def version_json() -> dict:
    """The 1.8.9 version manifest, fetched through the global manifest."""
    dest = paths.META / f"{paths.VERSION}.json"
    if dest.is_file():
        return json.loads(dest.read_text(encoding="utf-8"))

    manifest = fetch_json(MANIFEST_URL, paths.META / "version_manifest_v2.json")
    entry = next((v for v in manifest["versions"] if v["id"] == paths.VERSION), None)
    if entry is None:
        raise RuntimeError(f"{paths.VERSION} is not in Mojang's version manifest")
    return fetch_json(entry["url"], dest, entry.get("sha1"))


def asset_index(vj: dict) -> dict:
    ai = vj["assetIndex"]
    dest = paths.ASSETS / "indexes" / f"{ai['id']}.json"
    seed = _cache("assets" , "indexes", f"{ai['id']}.json")
    fetch(ai["url"], dest, ai.get("sha1"), ai.get("size"), seed)
    return json.loads(dest.read_text(encoding="utf-8"))


# ------------------------------------------------------------------- launcher cache


def _cache(*parts: str) -> Path | None:
    dm = paths.dot_minecraft()
    return dm.joinpath(*parts) if dm else None


# ------------------------------------------------------------------------- rules


def _rules_allow(entry: dict) -> bool:
    """Mojang's allow/disallow list, evaluated for Windows.  Last match wins."""
    rules = entry.get("rules")
    if not rules:
        return True
    allowed = False
    for rule in rules:
        os_spec = rule.get("os")
        if os_spec and os_spec.get("name", "windows") != "windows":
            continue
        allowed = rule.get("action") == "allow"
    return allowed


def _natives_classifier(lib: dict) -> str | None:
    natives = lib.get("natives")
    if not natives or "windows" not in natives:
        return None
    return natives["windows"].replace("${arch}", "64")


# --------------------------------------------------------------------- the pieces


def client_jar(vj: dict) -> Path:
    d = vj["downloads"]["client"]
    dest = paths.SHARED / f"{paths.VERSION}-client.jar"
    seed = _cache("versions", paths.VERSION, f"{paths.VERSION}.jar")
    fetch(d["url"], dest, d["sha1"], d["size"], seed)
    return dest


def libraries(vj: dict) -> tuple[list[Path], list[tuple[Path, list[str]]]]:
    """(classpath jars, native archives with their exclude lists), downloaded."""
    jobs: list[tuple] = []
    cp: list[Path] = []
    natives: list[tuple[Path, list[str]]] = []

    for lib in vj["libraries"]:
        if not _rules_allow(lib):
            continue
        downloads = lib.get("downloads", {})

        art = downloads.get("artifact")
        if art and "path" in art:
            dest = paths.LIBRARIES / art["path"]
            jobs.append((art["url"], dest, art.get("sha1"), art.get("size"),
                         _cache("libraries", *art["path"].split("/"))))
            cp.append(dest)

        classifier = _natives_classifier(lib)
        if classifier:
            nat = downloads.get("classifiers", {}).get(classifier)
            if nat and "path" in nat:
                dest = paths.LIBRARIES / nat["path"]
                jobs.append((nat["url"], dest, nat.get("sha1"), nat.get("size"),
                             _cache("libraries", *nat["path"].split("/"))))
                natives.append((dest, lib.get("extract", {}).get("exclude", ["META-INF/"])))

    fetch_many(jobs, "libraries")
    return cp, natives


def extract_natives(archives: list[tuple[Path, list[str]]]) -> Path:
    """LWJGL's .dll files, flattened into one directory for -Djava.library.path."""
    stamp = paths.NATIVES / ".extracted"
    key = "\n".join(sorted(a.name for a, _ in archives))
    if stamp.is_file() and stamp.read_text(encoding="utf-8") == key:
        return paths.NATIVES
    for archive, exclude in archives:
        unzip(archive, paths.NATIVES, exclude=exclude)
    stamp.write_text(key, encoding="utf-8")
    say(f"  natives: {len(list(paths.NATIVES.glob('*.dll')))} dll extracted")
    return paths.NATIVES


def assets(vj: dict) -> Path:
    index = asset_index(vj)
    objects = index["objects"]
    jobs = []
    total = 0
    for info in objects.values():
        h = info["hash"]
        rel = f"{h[:2]}/{h}"
        dest = paths.ASSETS / "objects" / rel
        jobs.append((f"https://resources.download.minecraft.net/{rel}", dest,
                     h, info.get("size"), _cache("assets", "objects", h[:2], h)))
        total += info.get("size", 0)
    say(f"  assets: {len(jobs)} objects, {human(total)}")
    fetch_many(jobs, "assets", workers=24)
    return paths.ASSETS


# ---------------------------------------------------------------------- entry point


def setup() -> dict:
    """Download every Mojang-side piece.  Returns the version json."""
    paths.ensure_tree()
    vj = version_json()
    say(f"* Minecraft {paths.VERSION} ({vj['type']}, released {vj['releaseTime'][:10]})")

    jar = client_jar(vj)
    say(f"  client jar: {human(jar.stat().st_size)}")

    _cp, native_archives = libraries(vj)
    extract_natives(native_archives)
    assets(vj)
    return vj


def classpath(vj: dict) -> list[Path]:
    """Library jars only -- the mapping's own jar is appended by the launcher."""
    out: list[Path] = []
    for lib in vj["libraries"]:
        if not _rules_allow(lib):
            continue
        art = lib.get("downloads", {}).get("artifact")
        if art and "path" in art:
            out.append(paths.LIBRARIES / art["path"])
    return out
