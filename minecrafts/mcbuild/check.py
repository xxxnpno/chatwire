"""Check chatwire's hand-written name table against the real 1.8.9 mappings.

`chatwire/src/chatwire/mapping.hpp` carries a triple for every name chatwire
touches, and its own header comment says why that is dangerous: a wrong OBF name
fails SILENTLY.  The class resolves to something real, the member lookup finds
nothing, and the feature that needed it just never works.  That is what `avq`
for GuiNewChat did for a whole release.

Now that all three jars exist, the table can simply be looked up.  Everything
here is offline -- `shared/mappings/1.8.9-mappings.json` plus the class files
themselves, no JVM and no running game -- so it is the check that can run before
anything is launched.

    python mc.py check

Exit status is non-zero when an entry is wrong, so it also works as the last
step of a build.
"""

from __future__ import annotations

import json
import re
import struct
import zipfile
from dataclasses import dataclass
from pathlib import Path

from . import paths
from .util import say

#: One pass over the whole header, in source order: a group header, a `name`
#: initialiser, or a bare brace.  Scanning tokens rather than lines is what lets
#: an initialiser wrap -- three of them do, and a line-at-a-time reader skipped
#: exactly those three while reporting success on the rest.  A checker that
#: silently covers less than it says it does is the bug it was written to catch.
TOKEN_RE = re.compile(
    r"""(?P<group>(?:namespace|struct)\s+(?P<gname>\w+))
      | (?P<entry>(?:inline\s+constexpr\s+)?name\s+(?P<id>\w+)\s*\{(?P<body>[^{}]*)\})
      | (?P<open>\{)
      | (?P<close>\})""",
    re.VERBOSE,
)
FIELD_RE = re.compile(r"\.(?P<key>mcp|obf|srg)\s*=\s*\"(?P<value>[^\"]*)\"")


@dataclass(slots=True)
class Entry:
    group: str
    ident: str
    mcp: str
    obf: str
    srg: str
    line: int

    @property
    def is_class(self) -> bool:
        return self.ident == "clazz"


# ------------------------------------------------------------------ mapping.hpp


def strip_comments(text: str) -> str:
    """Blank out comments, keeping every newline so line numbers still line up.

    mapping.hpp is mostly prose, and its prose quotes bytecode and JSON.  A
    brace in a comment would desynchronise the depth tracking below.
    """
    out: list[str] = []
    i, n = 0, len(text)
    while i < n:
        two = text[i:i + 2]
        if two == "//":
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif two == "/*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join(c if c == "\n" else " " for c in text[i:j]))
            i = j
        elif text[i] == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:min(j + 1, n)])
            i = min(j + 1, n)
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def parse_table(header: Path) -> list[Entry]:
    """Every `name` initialiser in mapping.hpp, tagged with its enclosing group."""
    text = strip_comments(header.read_text(encoding="utf-8"))
    out: list[Entry] = []
    stack: list[tuple[str, int]] = []
    pending: str | None = None
    depth = 0

    for m in TOKEN_RE.finditer(text):
        if m.group("group"):
            pending = m.group("gname")
        elif m.group("entry"):
            kv = {f.group("key"): f.group("value") for f in FIELD_RE.finditer(m.group("body"))}
            if kv:
                out.append(Entry(group=stack[-1][0] if stack else "?",
                                 ident=m.group("id"),
                                 line=text.count("\n", 0, m.start()) + 1,
                                 mcp=kv.get("mcp", ""), obf=kv.get("obf", ""),
                                 srg=kv.get("srg", "")))
        elif m.group("open"):
            depth += 1
            if pending is not None:
                stack.append((pending, depth))
                pending = None
        else:
            if stack and stack[-1][1] == depth:
                stack.pop()
            depth -= 1
    return out


# ------------------------------------------------------- the class hierarchy, raw

_CP_SKIP = {3: 4, 4: 4, 9: 4, 10: 4, 11: 4, 12: 4, 18: 4, 17: 4,
            8: 2, 7: 2, 16: 2, 19: 2, 20: 2, 15: 3, 5: 8, 6: 8}


def _super_of(data: bytes) -> str | None:
    """The super_class name of one .class file, straight out of the bytes.

    Only the constant pool, `this_class` and `super_class` are read -- enough to
    walk a hierarchy without a JVM, a decompiler or a javap subprocess.
    """
    if data[:4] != b"\xca\xfe\xba\xbe":
        return None
    pos = 8
    (count,) = struct.unpack_from(">H", data, pos)
    pos += 2
    utf8: dict[int, str] = {}
    class_ref: dict[int, int] = {}
    i = 1
    while i < count:
        tag = data[pos]
        pos += 1
        if tag == 1:
            (length,) = struct.unpack_from(">H", data, pos)
            utf8[i] = data[pos + 2:pos + 2 + length].decode("utf-8", "replace")
            pos += 2 + length
        else:
            if tag == 7:
                class_ref[i] = struct.unpack_from(">H", data, pos)[0]
            pos += _CP_SKIP.get(tag, 0)
            if tag in (5, 6):
                i += 1                                   # long/double eat two slots
        i += 1
    pos += 2 + 2                                          # access_flags, this_class
    (super_index,) = struct.unpack_from(">H", data, pos)
    if not super_index:
        return None
    return utf8.get(class_ref.get(super_index, 0))


class Hierarchy:
    """Superclass chains, read lazily out of the mcp jar."""

    def __init__(self, jar: Path) -> None:
        self._zip = zipfile.ZipFile(jar)
        self._names = set(self._zip.namelist())
        self._cache: dict[str, list[str]] = {}

    def chain(self, class_name: str) -> list[str]:
        if class_name in self._cache:
            return self._cache[class_name]
        out: list[str] = []
        current: str | None = class_name
        while current and f"{current}.class" in self._names and current not in out:
            out.append(current)
            current = _super_of(self._zip.read(f"{current}.class"))
        self._cache[class_name] = out
        return out


# ------------------------------------------------------------------- the check


def _index(doc: dict) -> tuple[dict, dict, dict]:
    by_class = {c["srg"]: c for c in doc["classes"]}
    members: dict[tuple[str, str], list[dict]] = {}
    for kind in ("fields", "methods"):
        for m in doc[kind]:
            members.setdefault((m["owner"], m["srg"]), []).append({**m, "kind": kind[:-1]})
    by_mcp: dict[tuple[str, str], list[dict]] = {}
    for (owner, _srg), entries in members.items():
        for e in entries:
            by_mcp.setdefault((owner, e["mcp"]), []).append(e)
    return by_class, members, by_mcp


def check(header: Path | None = None) -> int:
    header = header or (paths.ROOT.parent / "chatwire" / "src" / "chatwire" / "mapping.ixx")
    doc_path = paths.MAPPINGS / f"{paths.VERSION}-mappings.json"
    if not doc_path.is_file():
        raise SystemExit(f"{doc_path} is missing -- run `python mc.py setup` first")
    if not header.is_file():
        raise SystemExit(f"{header} is missing")

    doc = json.loads(doc_path.read_text(encoding="utf-8"))
    by_class, by_srg, by_mcp = _index(doc)
    entries = parse_table(header)
    hierarchy = Hierarchy(paths.mapping_jar("mcp"))

    say(f"* checking {len(entries)} entries in {header.name} against "
        f"{len(doc['classes'])} classes / {len(doc['fields'])} fields / "
        f"{len(doc['methods'])} methods")

    class_of_group: dict[str, str] = {
        e.group: e.mcp for e in entries if e.is_class
    }
    problems: list[str] = []

    def bad(e: Entry, message: str) -> None:
        problems.append(f"  {header.name}:{e.line}  {e.group}::{e.ident} -- {message}")

    # A group whose class carries only an mcp spelling and is not in the
    # mappings at all is a LIBRARY class -- com/mojang/authlib/GameProfile,
    # java/util/UUID.  Nobody remaps those, which is why they have one name, and
    # the MCP mappings do not describe them, which is why they cannot be checked
    # here.  Unverifiable is not the same as wrong, and reporting it as wrong
    # would train a reader to ignore this tool's output.
    library_groups = {
        e.group for e in entries
        if e.is_class and not e.obf and not e.srg and e.mcp not in by_class
    }
    for group in sorted(library_groups):
        say(f"  {group}: a library class, not remapped and not in the mappings -- skipped")

    for e in entries:
        if e.group in library_groups:
            continue
        if e.is_class:
            known = by_class.get(e.mcp)
            if known is None:
                bad(e, f"no class named '{e.mcp}' in 1.8.9")
            elif e.obf and e.obf != known["notch"]:
                bad(e, f"obf is '{e.obf}', the jar says '{known['notch']}'")
            continue

        owner = class_of_group.get(e.group)
        if owner is None:
            bad(e, f"group '{e.group}' has no clazz entry to look this up on")
            continue

        chain = hierarchy.chain(owner) or [owner]
        # Search by SRG name when there is one -- it is the unique key.  Fall
        # back to the MCP name for a member the table left srg-less.
        found = None
        for klass in chain:
            candidates = by_srg.get((klass, e.srg)) if e.srg else by_mcp.get((klass, e.mcp))
            if candidates:
                found = candidates[0]
                break
        if found is None:
            where = f"{owner} (or a superclass)" if len(chain) > 1 else owner
            bad(e, f"no {'srg ' + e.srg if e.srg else 'member ' + e.mcp} on {where}")
            continue
        if e.mcp and found["mcp"] != e.mcp:
            bad(e, f"mcp is '{e.mcp}', the mappings say '{found['mcp']}'")
        if e.obf and found["notch"] != e.obf:
            bad(e, f"obf is '{e.obf}', the jar says '{found['notch']}' "
                   f"on {found['notchOwner']}")

    if problems:
        say(f"* {len(problems)} problem(s):")
        for p in problems:
            say(p)
        return 1
    say(f"* all {len(entries) - sum(1 for e in entries if e.group in library_groups)} checkable entries agree with the jar")
    return 0
