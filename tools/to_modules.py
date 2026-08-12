#!/usr/bin/env python3
"""Turn chatwire's headers into C++26 module interface units.

Every `.hpp` becomes a `.ixx`:

    #pragma once                     ->  (nothing; a module needs no guard)
    #include <std headers>           ->  the global module fragment
    #include "chatwire/x.hpp"        ->  import chatwire.x;
    namespace chatwire::y { ... }    ->  export namespace chatwire::y { ... }

`chatwire/common.hpp` disappears.  It existed to give every header one include
line and one ordering rule; a module carries its own global module fragment, so
the list is written into each unit instead.  That is the one place the ordering
rule still matters and the only place a `#include` belongs.

WHAT IS NOT MECHANICAL, and is left for a human to look at afterwards:

  * a module cannot re-export what its fragment included, so a consumer needs
    its own `#include <memory>` (or an import) for `std::unique_ptr` in a
    signature it names;
  * `sdk` is the only unit that touches vmhook, and vmhook is itself a module
    now, so it `import`s rather than includes;
  * anything that must stay in the global module fragment because Win32 has no
    module: <windows.h>, <winsock2.h>, <bcrypt.h>.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path("C:/repos/cpp/chatwire/chatwire/src")

#: What common.hpp gave everybody.  Written into each unit's fragment, in the
#: order that file spent its comment explaining: standard library first, so a
#: platform header can never wrap a std declaration in `extern "C"`.
COMMON = """#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>
"""

#: header path (under chatwire/src) -> module name
MODULES = {
    "chatwire/ansi.hpp": "chatwire.ansi",
    "chatwire/chatwire.hpp": "chatwire.api",
    "chatwire/command_line.hpp": "chatwire.command_line",
    "chatwire/config.hpp": "chatwire.config",
    "chatwire/console.hpp": "chatwire.console",
    "chatwire/crypto.hpp": "chatwire.crypto",
    "chatwire/feature.hpp": "chatwire.feature",
    "chatwire/json.hpp": "chatwire.json",
    "chatwire/log.hpp": "chatwire.log",
    "chatwire/mapping.hpp": "chatwire.mapping",
    "chatwire/module.hpp": "chatwire.module",
    "chatwire/net.hpp": "chatwire.net",
    "chatwire/reflect.hpp": "chatwire.reflect",
    "chatwire/sdk.hpp": "chatwire.sdk",
    "chatwire/terminal.hpp": "chatwire.terminal",
    "chatwire/ws/websocket.hpp": "chatwire.ws.websocket",
    "chatwire/ws/server.hpp": "chatwire.ws.server",
    "chatwire/features/chat.hpp": "chatwire.features.chat",
    "chatwire/features/commands.hpp": "chatwire.features.commands",
    "chatwire/features/mapping.hpp": "chatwire.features.mapping",
    "chatwire/features/rewrite.hpp": "chatwire.features.rewrite",
    "chatwire/features/scoreboard.hpp": "chatwire.features.scoreboard",
    "chatwire/features/system.hpp": "chatwire.features.system",
    "chatwire/features/world.hpp": "chatwire.features.world",
    "entry.hpp": "chatwire.entry",
}

OWN_INCLUDE = re.compile(r'^\s*#include\s+"([^"]+)"\s*$', re.M)
SYS_INCLUDE = re.compile(r'^\s*#include\s+<([^>]+)>\s*$', re.M)


def convert(path: Path, name: str) -> str:
    text = path.read_text(encoding="utf-8")

    imports: list[str] = []
    fragment: list[str] = []

    def take_own(m: re.Match[str]) -> str:
        target = m.group(1)
        if target == "chatwire/common.hpp":
            return ""                                   # its content is the fragment
        if target in MODULES:
            imports.append(MODULES[target])
            return ""
        fragment.append(f'#include "{target}"')
        return ""

    def take_sys(m: re.Match[str]) -> str:
        header = m.group(1)
        if header == "vmhook/vmhook.hpp":
            imports.append("vmhook")
            return ""
        fragment.append(f"#include <{header}>")
        return ""

    text = OWN_INCLUDE.sub(take_own, text)
    text = SYS_INCLUDE.sub(take_sys, text)
    text = re.sub(r"^#pragma once\s*$\n?", "", text, flags=re.M)

    # Export every namespace the unit opens at column 0.
    text = re.sub(r"^namespace\s", "export namespace ", text, flags=re.M)

    head = "module;\n\n" + COMMON
    if fragment:
        head += "\n" + "\n".join(dict.fromkeys(fragment)) + "\n"
    head += f"\nexport module {name};\n"
    for dependency in dict.fromkeys(imports):
        head += f"import {dependency};\n"
    return head + "\n" + text.lstrip("\n")


def main() -> int:
    written = 0
    for relative, name in MODULES.items():
        source = ROOT / relative
        if not source.is_file():
            print(f"! missing {relative}", file=sys.stderr)
            continue
        target = source.with_suffix(".ixx")
        target.write_text(convert(source, name), encoding="utf-8")
        source.unlink()
        written += 1
    print(f"{written} module interface unit(s) written")

    # The three translation units that consume them.
    for tu in ("chatwire.cpp", "dllmain.cpp", "injector.cpp"):
        path = ROOT / tu
        if not path.is_file():
            continue
        text = path.read_text(encoding="utf-8")
        imports: list[str] = []

        def own(m: re.Match[str]) -> str:
            target = m.group(1)
            if target == "chatwire/common.hpp":
                return ""
            if target in MODULES:
                imports.append(MODULES[target])
                return ""
            return m.group(0)

        text = OWN_INCLUDE.sub(own, text)
        if imports:
            # AFTER every #include: a translation unit that imports before it
            # includes gets `redefinition of std::__is_constant_evaluated`.
            last = 0
            for m in re.finditer(r"^\s*#include\s+[<\"][^>\"]+[>\"]\s*$", text, re.M):
                last = m.end()
            block = "\n\n" + "\n".join(f"import {d};" for d in dict.fromkeys(imports)) + "\n"
            text = text[:last] + block + text[last:]
        path.write_text(text, encoding="utf-8")
    print("consumers rewritten")

    common = ROOT / "chatwire/common.hpp"
    if common.is_file():
        common.unlink()
        print("chatwire/common.hpp removed -- each module carries its own fragment")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
