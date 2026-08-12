#!/usr/bin/env python3
"""Move every state-holding function-local static into an implementation unit.

GCC 16.2 emits a function-local `static` from an INLINE function in a module's
purview into the module's object *and* into every consumer's.  The link only
succeeds with `-Wl,--allow-multiple-definition`, and that flag is harmless for
code and quietly catastrophic for state: the registrations in chatwire.cpp and
the lookups in the feature modules landed on different copies of the same
vector, so every feature registered and none was found.

The fix keeps construct-on-first-use exactly -- the property those functions
were written for -- and gives each one a single definition:

    interface (.ixx)   auto storage() noexcept -> std::vector<feature*>&;
    implementation     module chatwire.feature;
                       auto storage() noexcept -> ... { static ... }

A `.cpp` compiled once emits its statics once, whatever the compiler thinks
about inline functions in a module.
"""

from __future__ import annotations

import re
from pathlib import Path

ROOT = Path("C:/repos/cpp/chatwire/chatwire/src")

#: (interface file, module name, [(signature, body)])
UNITS = [
    ("chatwire/feature.ixx", "chatwire.feature", "chatwire/feature_impl.cpp",
     ["storage", "started"]),
    ("chatwire/log.ixx", "chatwire.log", "chatwire/log_impl.cpp",
     ["sink_mutex"]),
    ("chatwire/features/commands.ixx", "chatwire.features.commands",
     "chatwire/features/commands_impl.cpp", ["table", "table_mutex"]),
    ("chatwire/features/rewrite.ixx", "chatwire.features.rewrite",
     "chatwire/features/rewrite_impl.cpp", ["rules"]),
]

NOTE = """// The state the interface only DECLARES.
//
// Each of these was an inline function with a function-local static, which is
// the construct-on-first-use idiom and exactly right for a header.  In a module
// GCC 16.2 emits that static into the module's object AND into every consumer's,
// and `-Wl,--allow-multiple-definition` -- which the link needs anyway -- then
// silently gives each consumer its own copy.  Duplicated CODE is harmless;
// duplicated STATE meant the feature registry was written by one copy and read
// from another, so every feature registered and none was found.
//
// A .cpp is compiled once, so its statics exist once.  Nothing else changes:
// these are the same bodies, still constructed on first use, still deliberately
// leaked so a detour that outlives shutdown cannot use freed memory.
"""


def main() -> int:
    for interface, module, impl, names in UNITS:
        src = ROOT / interface
        text = src.read_text(encoding="utf-8")
        moved: list[str] = []

        for name in names:
            pattern = re.compile(
                r"^([ \t]*)(?:\[\[nodiscard\]\]\s*)?inline (auto " + name +
                r"\(\) noexcept -> [^\n{]+)\n\1\{\n(.*?)^\1\}\n",
                re.S | re.M)
            m = pattern.search(text)
            if not m:
                print(f"! {name} did not match in {interface}")
                return 1
            indent, signature, body = m.group(1), m.group(2).strip(), m.group(3)
            declaration = f"{indent}[[nodiscard]] {signature};\n"
            text = text[:m.start()] + declaration + text[m.end():]
            moved.append(f"{signature}\n{{\n" +
                         "".join(l[len(indent):] if l.startswith(indent) else l
                                 for l in body.splitlines(keepends=True)) + "}\n")

        src.write_text(text, encoding="utf-8")

        # Which namespace the definitions belong in.
        namespace = {
            "chatwire.feature": "chatwire::registry::detail",
            "chatwire.log": "chatwire::log::detail",
            "chatwire.features.commands": "chatwire::features::detail",
            "chatwire.features.rewrite": "chatwire::features::detail",
        }[module]

        (ROOT / impl).write_text(
            NOTE + f"\nmodule {module};\n\nnamespace {namespace}\n{{\n" +
            "\n".join("    " + b.replace("\n", "\n    ").rstrip() + "\n" for b in moved) +
            "}\n", encoding="utf-8")
        print(f"{module}: {', '.join(names)} -> {impl}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
