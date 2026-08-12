"""Widen access flags, because remapping moves classes into packages.

Mojang's obfuscator puts EVERY class in the default package: `ave`, `avt`, `bdb`.
Package-private and protected members are therefore reachable from everywhere,
and the shipped jar leans on that heavily.

Remapping to srg or mcp names undoes it.  `net/minecraft/client/gui/GuiScreen`
and `net/minecraft/client/multiplayer/GuiConnecting` used to be neighbours and
are now strangers, so an access the compiler allowed in 2015 becomes:

    java.lang.IllegalAccessError: tried to access field
    net.minecraft.client.gui.GuiScreen.field_146297_k
    from class net.minecraft.client.multiplayer.GuiConnecting$1

which is what happened here: both remapped clients reached the main menu, tried
to connect to a server, and died in the connection screen.  A remapped jar is
not finished until this pass has run -- it is why MCP's own pipeline has an
access-transform step between deobfuscation and anything that runs.

WHAT IS WIDENED, AND WHAT IS DELIBERATELY NOT.  Package-private and protected
become public, on classes, fields and methods.  `private` is left exactly as it
is, and that restraint is the whole safety argument: a private member cannot be
reached from another class in any package, so widening it fixes nothing -- while
making a private method public CAN change behaviour, because two classes in one
hierarchy that both declare a private `a()` suddenly have an override
relationship they never had.  Obfuscated Minecraft is full of one-letter names,
so that is not a theoretical collision.

The class file is patched in place: only two-byte access flags change, the
constant pool is untouched, and every offset stays where it was.
"""

from __future__ import annotations

import shutil
import struct
import zipfile
from pathlib import Path

from .util import say

ACC_PUBLIC = 0x0001
ACC_PRIVATE = 0x0002
ACC_PROTECTED = 0x0004

#: Constant-pool tag -> bytes to skip after the tag.  Long and Double also eat a
#: second pool slot, which is handled at the call site.
_CP_SKIP = {3: 4, 4: 4, 5: 8, 6: 8, 7: 2, 8: 2, 9: 4, 10: 4, 11: 4, 12: 4,
            15: 3, 16: 2, 17: 4, 18: 4, 19: 2, 20: 2}


def _end_of_constant_pool(data: bytes) -> int:
    """The offset just past the constant pool, i.e. where access_flags begins."""
    (count,) = struct.unpack_from(">H", data, 8)
    pos = 10
    i = 1
    while i < count:
        tag = data[pos]
        pos += 1
        if tag == 1:                                   # CONSTANT_Utf8
            (length,) = struct.unpack_from(">H", data, pos)
            pos += 2 + length
        else:
            pos += _CP_SKIP.get(tag, 0)
            if tag in (5, 6):
                i += 1
        i += 1
    return pos


def _skip_attributes(data: bytes, pos: int) -> int:
    (count,) = struct.unpack_from(">H", data, pos)
    pos += 2
    for _ in range(count):
        (length,) = struct.unpack_from(">I", data, pos + 2)
        pos += 6 + length
    return pos


def _widen(flags: int) -> int:
    """public unless private.  See the module note for why private is untouched."""
    if flags & ACC_PRIVATE:
        return flags
    return (flags & ~ACC_PROTECTED) | ACC_PUBLIC


def widen_class(data: bytes) -> tuple[bytes, int]:
    """Return the patched class file and how many flags changed."""
    if data[:4] != b"\xca\xfe\xba\xbe":
        return data, 0
    out = bytearray(data)
    changed = 0

    pos = _end_of_constant_pool(data)

    (class_flags,) = struct.unpack_from(">H", out, pos)
    if not class_flags & ACC_PUBLIC:
        struct.pack_into(">H", out, pos, class_flags | ACC_PUBLIC)
        changed += 1
    pos += 2 + 2 + 2                                   # access, this, super

    (interfaces,) = struct.unpack_from(">H", out, pos)
    pos += 2 + interfaces * 2

    for _ in range(2):                                 # fields, then methods
        (count,) = struct.unpack_from(">H", out, pos)
        pos += 2
        for _ in range(count):
            (flags,) = struct.unpack_from(">H", out, pos)
            widened = _widen(flags)
            if widened != flags:
                struct.pack_into(">H", out, pos, widened)
                changed += 1
            pos += 6                                   # access, name, descriptor
            pos = _skip_attributes(out, pos)

    return bytes(out), changed


def widen_jar(source: Path, dest: Path) -> int:
    """Copy `source` to `dest`, widening every class on the way through."""
    changed = 0
    classes = 0
    tmp = dest.with_suffix(".jar.widening")
    with zipfile.ZipFile(source) as src, \
            zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as out:
        for info in src.infolist():
            data = src.read(info.filename)
            if info.filename.endswith(".class"):
                data, n = widen_class(data)
                changed += n
                classes += 1
            # A fresh ZipInfo would lose the entry's timestamp; reusing this one
            # keeps the jar byte-comparable across runs apart from the flags.
            out.writestr(info, data)
    shutil.move(str(tmp), str(dest))
    say(f"  access: {changed} flags widened across {classes} classes")
    return changed
