#!/bin/sh
# Release builds of projectdream_builder: one self-contained program per platform (nothing to install), each zipped
# with projectdream_nx.nro so the folder it writes is ready for the SD card.
#   [RIDS="win-x64 ..."] tools/builder/publish.sh <projectdream_nx.nro> <output folder> [version]
# Needs the .NET SDK (10 or newer). On macOS the Mac builds come out ad-hoc signed (Apple Silicon runs nothing
# unsigned); players still confirm the first start (right-click, Open).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
nro=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
mkdir -p "$2"; out=$(cd "$2" && pwd)
version=${3:-1.0.0}
dotnet=${DOTNET:-dotnet}
command -v "$dotnet" >/dev/null 2>&1 || dotnet="$HOME/.dotnet/dotnet"
for rid in ${RIDS:-win-x64 osx-arm64 osx-x64 linux-x64 linux-arm64}; do
  stage="$out/stage/$rid"
  rm -rf "$stage"
  "$dotnet" publish "$here/ProjectDreamBuilder.csproj" -c Release -r "$rid" -o "$stage" -p:Version="$version" -v q --nologo
  cp "$nro" "$stage/"
  cat > "$stage/README.txt" <<EOF
projectdream_builder $version ($rid)

Makes sdt.apk, the game file of projectdream_nx (Sonic Dream Team on the Nintendo Switch), from your own macOS
Sonic Dream Team 1.10.1 and your own Project_Dream.exe. It downloads Unity's Android player (about 675 MB) from Unity.

1. Start projectdream_builder. macOS: if it is blocked the first time, System Settings, Privacy & Security, Open
   Anyway. Linux: start it from a terminal.
2. Give it your Sonic Dream Team (.app or .zip) and Project_Dream.exe (drag them onto the window, then Enter).
3. When it is done, copy the switch folder in SD_CARD (next to the builder) to the root of your SD card.

It needs about 6 GB free while it works. https://github.com/aks796/projectdream_nx
EOF
  rm -f "$out/projectdream_builder_${version}_$rid.zip"
  (cd "$stage" && zip -q -r -X "$out/projectdream_builder_${version}_$rid.zip" .)
  ls -la "$out/projectdream_builder_${version}_$rid.zip"
  # each platform's build output is ~100 MB: gone before the next one
  rm -rf "$stage" "$here/bin/Release/net10.0/$rid" "$here/obj/Release/net10.0/$rid"
done
rm -rf "$out/stage"
