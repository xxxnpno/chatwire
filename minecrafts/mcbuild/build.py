"""Turn one obfuscated client jar into three jars that all still run.

    vanilla/1.8.9-vanilla.jar   the shipped jar, byte for byte
    srg/1.8.9-srg.jar           vanilla remapped through joined.srg
    mcp/1.8.9-mcp.jar           srg remapped through srg-to-mcp.srg

The mcp jar is built from the srg jar rather than from vanilla because MCP does
not name classes -- it only names the members of classes srg already renamed.
"""

from __future__ import annotations

import shutil
from pathlib import Path

from . import mappings, paths, tools, vanilla
from .util import human, run, say


def _remap(src: Path, dst: Path, srg: Path, label: str) -> Path:
    if dst.is_file() and dst.stat().st_mtime > max(src.stat().st_mtime, srg.stat().st_mtime):
        say(f"  {label}: up to date")
        return dst
    dst.parent.mkdir(parents=True, exist_ok=True)
    tmp = dst.with_suffix(".jar.tmp")
    say(f"  {label}: remapping {src.name} through {srg.name}")
    run([
        str(paths.java_tools()), "-Xmx2G", "-jar", str(tools.special_source()),
        "--in-jar", str(src),
        "--out-jar", str(tmp),
        "--srg-in", str(srg),
        "--quiet",
    ])
    tmp.replace(dst)
    say(f"  {label}: {human(dst.stat().st_size)}")
    return dst


def build(which: tuple[str, ...] = paths.MAPPINGS_ORDER) -> dict[str, Path]:
    paths.ensure_tree()
    vj = vanilla.version_json()
    client = vanilla.client_jar(vj)
    out: dict[str, Path] = {}

    vanilla_jar = paths.mapping_jar("vanilla")
    if "vanilla" in which:
        if not vanilla_jar.is_file() or vanilla_jar.stat().st_size != client.stat().st_size:
            vanilla_jar.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(client, vanilla_jar)
        say(f"  vanilla: {human(vanilla_jar.stat().st_size)}")
        out["vanilla"] = vanilla_jar

    srg_jar = paths.mapping_jar("srg")
    if "srg" in which or "mcp" in which:
        if not vanilla_jar.is_file():
            shutil.copyfile(client, vanilla_jar)
        _remap(vanilla_jar, srg_jar, mappings.notch_to_srg(), "srg")
        if "srg" in which:
            out["srg"] = srg_jar

    if "mcp" in which:
        mcp_jar = paths.mapping_jar("mcp")
        _remap(srg_jar, mcp_jar, mappings.srg_to_mcp(), "mcp")
        out["mcp"] = mcp_jar

    return out
