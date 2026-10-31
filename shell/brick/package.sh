#!/bin/sh
# Assemble build-brick/DC-DEADCAST.pak from the Brick build (shell/brick/build.sh), shell/brick/pak and
# pak.json (at the repository's root, where the Pak Store reads it):
# the folder NextUI runs for games in Roms/<name> (DC-DEADCAST).
set -e
cd "$(dirname "$0")/../.."
EXE=build-brick/deadcast/deadcast
PAK=build-brick/DC-DEADCAST.pak
[ -x "$EXE" ] || { echo "package: $EXE missing (shell/brick/build.sh)"; exit 2; }
rm -rf "$PAK"
mkdir -p "$PAK/config" "$PAK/licenses" "$PAK/bios"
cp shell/brick/pak/launch.sh pak.json "$PAK/"
cp shell/brick/pak/config/* "$PAK/config/"
llvm-strip -o "$PAK/deadcast" "$EXE"
chmod +x "$PAK/launch.sh" "$PAK/deadcast"
cp LICENSE "$PAK/licenses/flycast-GPL-2.0.txt"
echo "package: $PAK ($(du -sh "$PAK" | cut -f1))"
