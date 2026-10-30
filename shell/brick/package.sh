#!/bin/sh
# Assemble build-brick/DC-Deadcast.pak from the Brick build (shell/brick/build.sh) and shell/brick/pak:
# the folder NextUI runs for games in Roms/<name> (DC-Deadcast).
set -e
cd "$(dirname "$0")/../.."
EXE=build-brick/deadcast/deadcast
PAK=build-brick/DC-Deadcast.pak
[ -x "$EXE" ] || { echo "package: $EXE missing (shell/brick/build.sh)"; exit 2; }
rm -rf "$PAK"
mkdir -p "$PAK/config" "$PAK/licenses"
cp shell/brick/pak/launch.sh shell/brick/pak/pak.json "$PAK/"
cp shell/brick/pak/config/* "$PAK/config/"
llvm-strip -o "$PAK/deadcast" "$EXE"
chmod +x "$PAK/launch.sh" "$PAK/deadcast"
cp LICENSE "$PAK/licenses/flycast-GPL-2.0.txt"
echo "package: $PAK ($(du -sh "$PAK" | cut -f1))"
