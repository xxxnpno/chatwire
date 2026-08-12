"""The two Java tools we shell out to: a remapper and a decompiler."""

from __future__ import annotations

from pathlib import Path

from . import paths
from .util import fetch, say

SPECIALSOURCE_VERSION = "1.11.0"
SPECIALSOURCE_URL = (
    "https://repo1.maven.org/maven2/net/md-5/SpecialSource/"
    f"{SPECIALSOURCE_VERSION}/SpecialSource-{SPECIALSOURCE_VERSION}-shaded.jar"
)

VINEFLOWER_VERSION = "1.12.0"
VINEFLOWER_URL = (
    "https://github.com/Vineflower/vineflower/releases/download/"
    f"{VINEFLOWER_VERSION}/vineflower-{VINEFLOWER_VERSION}.jar"
)


def special_source() -> Path:
    dest = paths.TOOLS / f"SpecialSource-{SPECIALSOURCE_VERSION}.jar"
    if not dest.is_file():
        say(f"  tool: SpecialSource {SPECIALSOURCE_VERSION}")
    return fetch(SPECIALSOURCE_URL, dest)


def vineflower() -> Path:
    dest = paths.TOOLS / f"vineflower-{VINEFLOWER_VERSION}.jar"
    if not dest.is_file():
        say(f"  tool: Vineflower {VINEFLOWER_VERSION}")
    return fetch(VINEFLOWER_URL, dest)
