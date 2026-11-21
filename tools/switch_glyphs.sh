#!/bin/sh
# The Switch's button icons (L, R, +, -) made from the game's own (tools/switch_glyphs.py) into ../sdt_out/glyphs,
# from ../sdt_out/sdt.apk; tools/package.sh puts them next to the NRO. A venv for UnityPy and Pillow in
# ../sdt_out/pyvenv (made the first time).
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
OUT=$(cd "$HERE/.." && pwd)/sdt_out
if [ ! -x "$OUT/pyvenv/bin/python" ]; then
  python3 -m venv "$OUT/pyvenv"
  "$OUT/pyvenv/bin/pip" install -q UnityPy Pillow
fi
"$OUT/pyvenv/bin/python" -I "$HERE/tools/switch_glyphs.py" "$OUT/sdt.apk" "$OUT/glyphs"
