#!/usr/bin/env python3
"""merge_dksh_cache.py <out> <cache> [<cache> ...]: vkdk shader cache files (internal/shaders/dksh.cache, a
shaders.pack made by this script) merged into one, each variant once.

The result is what sdt_nx reads as its prebuilt set (<port folder>/shaders.pack): the variants a play session
compiled, so a console that has not played that far yet reads them instead of compiling them on first sight.
The format is vkdk's cache file (vkdk_pipeline.cpp): "VKDKCA1\\0", then records {u64 key, u32 size, u32 0,
DKSH, zero padding to 8}; a record cut short ends a file.

The variants are compiled from the game's shaders: a pack is for the player's own console, not for sharing.
"""
import struct
import sys

MAGIC = b"VKDKCA1\0"


def records(path):
    d = open(path, "rb").read()
    if d[:8] != MAGIC:
        print("%s: not a vkdk shader cache" % path, file=sys.stderr)
        return
    at = 8
    while at + 16 <= len(d):
        key, size, _ = struct.unpack_from("<QII", d, at)
        data = at + 16
        nxt = data + ((size + 7) & ~7)
        if size < 24 or d[data:data + 4] != b"DKSH" or nxt > len(d):
            break
        yield key, d[data:data + size]
        at = nxt


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    seen = {}
    for p in sys.argv[2:]:
        n = 0
        for key, dksh in records(p):
            if key not in seen:
                seen[key] = dksh
                n += 1
        print("%s: %d new variants" % (p, n))
    with open(sys.argv[1], "wb") as f:
        f.write(MAGIC)
        for key in sorted(seen):
            dksh = seen[key]
            f.write(struct.pack("<QII", key, len(dksh), 0))
            f.write(dksh)
            f.write(b"\0" * (((len(dksh) + 7) & ~7) - len(dksh)))
    print("%s: %d variants" % (sys.argv[1], len(seen)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
