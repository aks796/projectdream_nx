#!/bin/sh
# Short check in Ryubing with this port's own profile (../ryubing: keys, firmware and config cloned from the user's
# Ryubing folder; its SD card holds sdt.apk as an APFS clone of ../sdt_out/sdt.apk): copy the fresh .nro in, run for
# N seconds, stop that instance and print the end of debug.log. Ryubing runs as a renamed clone (SDTEmu.app, program
# sdtemu: other sessions' clean-ups kill every process with "Ryujinx" in its command line). Emulator timings are not
# console timings. From mkt_nx's.
#
# usage: tools/ryubing_boot.sh [seconds]
HERE=$(cd "$(dirname "$0")/.." && pwd)
PROFILE=${PROFILE:-$(cd "$HERE/.." && pwd)/ryubing}
NAME=projectdream_nx
DEST="$PROFILE/sdcard/switch/$NAME"
SECS=${1:-60}

for pid in $(pgrep -f -- "SDTEmu.app/Contents/MacOS/sdtemu --root-data-dir"); do kill -TERM "$pid" 2>/dev/null; done
sleep 1
mkdir -p "$DEST"
cp "$HERE/$NAME.nro" "$DEST/"
rm -f "$DEST/debug.log"

if [ ! -d "$PROFILE/SDTEmu.app" ]; then
  cp -cR /Applications/Ryujinx.app "$PROFILE/SDTEmu.app"
  mv "$PROFILE/SDTEmu.app/Contents/MacOS/Ryujinx" "$PROFILE/SDTEmu.app/Contents/MacOS/sdtemu"
  /usr/libexec/PlistBuddy -c "Set :CFBundleExecutable sdtemu" -c "Set :CFBundleIdentifier local.sdt.emu" \
    "$PROFILE/SDTEmu.app/Contents/Info.plist"
  # (signed again with its entitlements: the hypervisor's among them)
  codesign -d --entitlements - --xml /Applications/Ryujinx.app > "$PROFILE/entitlements.plist" 2>/dev/null
  codesign --force -s - --entitlements "$PROFILE/entitlements.plist" "$PROFILE/SDTEmu.app/Contents/MacOS/sdtemu"
  codesign --force -s - --entitlements "$PROFILE/entitlements.plist" "$PROFILE/SDTEmu.app"
fi
open -n -a "$PROFILE/SDTEmu.app" --stdout "$PROFILE/launch.log" --stderr "$PROFILE/launch.log" \
  --args --root-data-dir "$PROFILE" "$DEST/$NAME.nro"
sleep "$SECS"
for pid in $(pgrep -f -- "SDTEmu.app/Contents/MacOS/sdtemu --root-data-dir"); do kill -TERM "$pid" 2>/dev/null; done
sleep 2
for pid in $(pgrep -f -- "SDTEmu.app/Contents/MacOS/sdtemu --root-data-dir"); do kill -9 "$pid" 2>/dev/null; done

echo "=== debug.log (last 150 lines)"
tail -150 "$DEST/debug.log" 2>/dev/null
echo "=== ryubing: errors and guest crashes"
grep -a -E "\|E\| |Unhandled|InvalidMemoryRegion|Guest stack trace" "$PROFILE/launch.log" | grep -v "mvk-info" | tail -40
