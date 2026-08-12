"""The three name systems for 1.8.9, and how to get from one to the next.

    notch      a.b()           obfuscated, what the shipped jar actually contains
      |  joined.srg  (MCP, per Minecraft version)
    srg        func_71407_l()  stable across 1.8.x, machine-generated
      |  fields.csv / methods.csv  (mcp_stable-22-1.8.9)
    mcp        runTick()       hand-written, what tutorials quote

Only the notch -> srg step renames classes; `a` becomes
`net/minecraft/util/EnumChatFormatting` and then stays put.  srg -> mcp is a
pure member rename, which is why the mcp jar is built from the srg jar and not
from the vanilla one.
"""

from __future__ import annotations

import csv
import io
import json
from dataclasses import dataclass, field
from pathlib import Path

from . import paths
from .util import fetch, find_in_zip, read_in_zip, say

FORGE_MAVEN = "https://maven.minecraftforge.net"
SRG_URL = f"{FORGE_MAVEN}/de/oceanlabs/mcp/mcp/{paths.VERSION}/mcp-{paths.VERSION}-srg.zip"

#: mcp_stable-22 is the last stable MCP channel published for 1.8.9.
MCP_CHANNEL = f"22-{paths.VERSION}"
CSV_URL = f"{FORGE_MAVEN}/de/oceanlabs/mcp/mcp_stable/{MCP_CHANNEL}/mcp_stable-{MCP_CHANNEL}.zip"


# ------------------------------------------------------------------------ download


def srg_zip() -> Path:
    return fetch(SRG_URL, paths.MAPPINGS / f"mcp-{paths.VERSION}-srg.zip")


def csv_zip() -> Path:
    return fetch(CSV_URL, paths.MAPPINGS / f"mcp_stable-{MCP_CHANNEL}.zip")


def notch_to_srg() -> Path:
    """`joined.srg` -- notch names on the left, srg names on the right."""
    dest = paths.MAPPINGS / "joined.srg"
    if not dest.is_file():
        z = srg_zip()
        name = find_in_zip(z, "joined.srg")
        if name is None:
            raise RuntimeError(f"no joined.srg inside {z}")
        dest.write_bytes(read_in_zip(z, name))
    return dest


def _csv_map(basename: str) -> dict[str, str]:
    z = csv_zip()
    name = find_in_zip(z, basename)
    if name is None:
        raise RuntimeError(f"no {basename} inside {z}")
    text = read_in_zip(z, name).decode("utf-8")
    out: dict[str, str] = {}
    for row in csv.DictReader(io.StringIO(text)):
        key = row.get("searge") or row.get("param")
        if key and row.get("name"):
            out[key] = row["name"]
    return out


# -------------------------------------------------------------------------- model


@dataclass(slots=True)
class Table:
    """joined.srg, parsed."""

    classes: dict[str, str] = field(default_factory=dict)          # notch -> srg
    #: (notch owner, notch name) -> (srg owner, srg name)
    fields: dict[tuple[str, str], tuple[str, str]] = field(default_factory=dict)
    #: (notch owner, notch name, notch desc) -> (srg owner, srg name, srg desc)
    methods: dict[tuple[str, str, str], tuple[str, str, str]] = field(default_factory=dict)


def parse_joined(path: Path | None = None) -> Table:
    path = path or notch_to_srg()
    t = Table()
    for line in path.read_text(encoding="utf-8").splitlines():
        kind, _, rest = line.partition(": ")
        parts = rest.split()
        if kind == "CL" and len(parts) == 2:
            t.classes[parts[0]] = parts[1]
        elif kind == "FD" and len(parts) == 2:
            lo, _, ln = parts[0].rpartition("/")
            ro, _, rn = parts[1].rpartition("/")
            t.fields[(lo, ln)] = (ro, rn)
        elif kind == "MD" and len(parts) == 4:
            lo, _, ln = parts[0].rpartition("/")
            ro, _, rn = parts[2].rpartition("/")
            t.methods[(lo, ln, parts[1])] = (ro, rn, parts[3])
    return t


# --------------------------------------------------------------- generated mapping


def srg_to_mcp() -> Path:
    """A .srg that renames srg members to MCP names and leaves classes alone.

    SpecialSource wants both sides fully qualified, so every class shows up as an
    identity CL line -- that is also what makes the file readable on its own.
    """
    dest = paths.MAPPINGS / "srg-to-mcp.srg"
    joined = notch_to_srg()
    if dest.is_file() and dest.stat().st_mtime > joined.stat().st_mtime:
        return dest

    t = parse_joined(joined)
    fields_csv = _csv_map("fields.csv")
    methods_csv = _csv_map("methods.csv")

    out: list[str] = []
    for srg_class in sorted(set(t.classes.values())):
        out.append(f"CL: {srg_class} {srg_class}")

    renamed_f = renamed_m = 0
    for owner, name in sorted(t.fields.values()):
        mcp = fields_csv.get(name)
        if mcp and mcp != name:
            out.append(f"FD: {owner}/{name} {owner}/{mcp}")
            renamed_f += 1
    for owner, name, desc in sorted(t.methods.values()):
        mcp = methods_csv.get(name)
        if mcp and mcp != name:
            out.append(f"MD: {owner}/{name} {desc} {owner}/{mcp} {desc}")
            renamed_m += 1

    dest.write_text("\n".join(out) + "\n", encoding="utf-8")
    say(f"  srg->mcp: {len(t.classes)} classes, {renamed_f} fields, {renamed_m} methods named")
    return dest


def unified() -> Path:
    """One JSON holding all three names for every class, field and method.

    This is the file chatwire reads: given any name in any mapping, it can find
    the notch name the running jar actually has.
    """
    dest = paths.MAPPINGS / f"{paths.VERSION}-mappings.json"
    joined = notch_to_srg()
    if dest.is_file() and dest.stat().st_mtime > joined.stat().st_mtime:
        return dest

    t = parse_joined(joined)
    fields_csv = _csv_map("fields.csv")
    methods_csv = _csv_map("methods.csv")

    doc = {
        "version": paths.VERSION,
        "mcpChannel": f"stable_{MCP_CHANNEL.split('-')[0]}",
        "classes": [
            {"notch": n, "srg": s, "mcp": s} for n, s in sorted(t.classes.items())
        ],
        "fields": [
            {
                "notchOwner": lo, "notch": ln,
                "owner": ro, "srg": rn, "mcp": fields_csv.get(rn, rn),
            }
            for (lo, ln), (ro, rn) in sorted(t.fields.items())
        ],
        "methods": [
            {
                "notchOwner": lo, "notch": ln, "notchDesc": ld,
                "owner": ro, "srg": rn, "mcp": methods_csv.get(rn, rn), "desc": rd,
            }
            for (lo, ln, ld), (ro, rn, rd) in sorted(t.methods.items())
        ],
    }
    dest.write_text(json.dumps(doc, separators=(",", ":")), encoding="utf-8")
    say(f"  unified: {len(doc['classes'])} classes, {len(doc['fields'])} fields, "
        f"{len(doc['methods'])} methods -> {dest.name}")
    return dest


def setup() -> None:
    paths.ensure_tree()
    srg_zip()
    csv_zip()
    notch_to_srg()
    srg_to_mcp()
    unified()
