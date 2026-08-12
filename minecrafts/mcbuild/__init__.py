"""Build and launch Minecraft 1.8.9 in all three of its mappings.

Nothing here is chatwire-specific: it downloads what Mojang and Forge publish,
remaps the client jar twice, and starts it.  chatwire uses the result as three
interchangeable targets to attach to.
"""

from . import build, decompile, launch, mappings, paths, tools, vanilla  # noqa: F401

__all__ = ["build", "decompile", "launch", "mappings", "paths", "tools", "vanilla"]
