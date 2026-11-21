#!/bin/sh
# SD_CARD/switch/projectdream_nx: the NRO; sdt.apk hard-linked from ../sdt_out (tools/prepare's output) when it is there;
# shaders.pack from ../sdt_out when there (the shader variants a play session compiled, merged with
# tools/merge_dksh_cache.py: read instead of compiled on the console's first play; built from the game's shaders,
# so it stays with the player's own files, never in the zip).
# glyphs/: the Switch's button icons made from the game's (tools/switch_glyphs.sh), game-derived too.
# Rebuilds only SD_CARD/switch (SD_CARD/debug holds hardware logs). SD_CARD_projectdream_nx.zip: the NRO alone.
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
OUT="$HERE/SD_CARD/switch/projectdream_nx"
mkdir -p "$OUT" "$HERE/SD_CARD/debug"
cp "$HERE/projectdream_nx.nro" "$OUT/projectdream_nx.nro"
if [ -f "$HERE/../sdt_out/sdt.apk" ]; then
  ln -f "$HERE/../sdt_out/sdt.apk" "$OUT/sdt.apk"
fi
if [ -f "$HERE/../sdt_out/shaders.pack" ]; then
  cp "$HERE/../sdt_out/shaders.pack" "$OUT/shaders.pack"
fi
# the Switch's button icons (tools/switch_glyphs.sh): game-derived like the APK, with the player's files only
if ls "$HERE/../sdt_out/glyphs/"*.bundle >/dev/null 2>&1; then
  mkdir -p "$OUT/glyphs"
  cp "$HERE/../sdt_out/glyphs/"*.bundle "$OUT/glyphs/"
fi
( cd "$HERE/SD_CARD" && rm -f ../SD_CARD_projectdream_nx.zip && zip -q -r ../SD_CARD_projectdream_nx.zip switch/projectdream_nx/projectdream_nx.nro )
md5 -q "$OUT/projectdream_nx.nro" | cut -c1-8 | sed 's/^/projectdream_nx.nro md5 /'
ls -la "$OUT"
