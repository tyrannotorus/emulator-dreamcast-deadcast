#!/bin/sh
# Build a recomp module (recomp/modules/MODULE) for NextUI's tg5040 handhelds from the builder's own disc, into
# build-brick/recomp/MODULE/: its executable, its manifest (module.cfg), its game's config and licences,
# what Deadcast keeps in <DEADCAST_HOME>/recomp/MODULE/ (frontend/recomp.h).
#   shell/brick/recomp.sh rez "/path/to/Rez (Europe) (En,Ja,Fr,De,Es,It).chd"
# Needs, besides Deadcast's own (shell/brick/build.sh): the device's libvulkan.so.1
# (shell/brick/devlibs.sh SERIAL), Python 3.11+, glslc and llvm-strip. GEN_OPT=-O2 (or -O3) compiles
# the translated code at that level instead of -O1. Arguments after the disc go to ninja.
set -e
cd "$(dirname "$0")/../.."
M=${1:?usage: shell/brick/recomp.sh MODULE DISC}
DISC=${2:?usage: shell/brick/recomp.sh MODULE DISC}
shift 2
[ -f "recomp/modules/$M/module.cfg" ] || { echo "recomp: no recomp/modules/$M/module.cfg"; exit 2; }
TC="$PWD/build-brick/toolchain"
[ -d "$TC/aarch64-nextui-linux-gnu/libc" ] || { echo "recomp: no toolchain (shell/brick/toolchain.sh)"; exit 2; }
VKLIB="$PWD/build-brick/devlibs/libvulkan.so.1"
CURLLIB="$PWD/build-brick/devlibs/libcurl.so.4"
for lib in "$VKLIB" "$CURLLIB"; do
    [ -f "$lib" ] || { echo "recomp: no $lib (shell/brick/devlibs.sh SERIAL)"; exit 2; }
done
for dep in freetype; do
    [ -n "$(ls -A "core/deps/$dep" 2>/dev/null)" ] || { echo "recomp: core/deps/$dep is empty (git submodule update --init core/deps/$dep)"; exit 2; }
done

# the translator, for this machine
H=build-brick/recomp-host
cmake -S recomp -B "$H" -G Ninja -DCMAKE_BUILD_TYPE=Release -DDREAM_RENDERER=OFF \
    -DCMAKE_DISABLE_FIND_PACKAGE_SDL3=ON > "$H.configure.log"
# and libchdr, through which dc.py reads the disc
ninja -C "$H" dream-translate chdr
TRANSLATE="$PWD/$H/engine/translator/dream-translate"
LIBCHDR="$(find "$PWD/$H" -name 'libchdr.so*' -type f | head -n 1)"

# the disc: checked against the module's game config, its boot file and code overlays extracted
# (recomp/modules/MODULE/game/extracted: the game's own code, never in git)
DC_PORT="$PWD/recomp/modules/$M" DCDISC_LIBCHDR="$LIBCHDR" python3 recomp/tools/dc.py setup --disc "$DISC"

# the module, for the handheld; pkg-config sees the sysroot's packages only, never this machine's
SYSROOT="$TC/aarch64-nextui-linux-gnu/libc"
export PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/pkgconfig:$SYSROOT/usr/share/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
unset PKG_CONFIG_PATH
# the build tree beside the packages (build-brick/recomp/MODULE), never in it
B=build-brick/recomp-build
mkdir -p build-brick/include
rm -rf build-brick/include/curl build-brick/include/vulkan build-brick/include/vk_video
cp -r /usr/include/curl /usr/include/vulkan /usr/include/vk_video build-brick/include/
cmake -S recomp -B "$B" -G Ninja \
    -DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG" -DCMAKE_C_FLAGS_RELEASE="-O3 -DNDEBUG" \
    -DCMAKE_TOOLCHAIN_FILE="$PWD/recomp/cmake/toolchains/tg5040.cmake" \
    -DDREAMCOMP_TG5040_ROOT="$TC" \
    -DCMAKE_BUILD_TYPE=Release \
    -DDREAMCOMP_NEXTUI=ON \
    -DDREAM_RENDERER=ON \
    -DVulkan_INCLUDE_DIR="$PWD/build-brick/include" \
    -DVulkan_LIBRARY="$VKLIB" \
    -DCMAKE_DISABLE_FIND_PACKAGE_SDL3=ON \
    -DDREAM_TRANSLATE_EXECUTABLE="$TRANSLATE" \
    -DDREAM_GEN_OPT="${GEN_OPT:--O1}" \
    -DCURL_INCLUDE_DIR="$PWD/build-brick/include" \
    -DCURL_LIBRARY="$CURLLIB" > "$B.configure.log"
ninja -C "$B" "$@" "${M}_boot"

# packaged as Deadcast installs it
EXEC=$(sed -n 's/^exec *= *//p' "recomp/modules/$M/module.cfg")
OUT="build-brick/recomp/$M"
rm -rf "$OUT"
mkdir -p "$OUT/licenses"
llvm-strip -o "$OUT/$EXEC" "$B/modules/$M/game/$EXEC"
chmod +x "$OUT/$EXEC"
cp "recomp/modules/$M/module.cfg" "$OUT/"
cp "recomp/modules/$M/game/$M.toml" "$OUT/"
[ -f "recomp/modules/$M/game/$M-pipelines.txt" ] && cp "recomp/modules/$M/game/$M-pipelines.txt" "$OUT/"
cp recomp/LICENSE "$OUT/licenses/deadcast-recomp-GPL-2.0.txt"
cp recomp/THIRD_PARTY_NOTICES.md "$OUT/licenses/THIRD_PARTY_NOTICES.md"
cp recomp/engine/third_party/libchdr/LICENSE.txt "$OUT/licenses/libchdr-BSD-3-Clause.txt"
cp recomp/engine/third_party/libchdr/deps/lzma-26.02/LICENSE "$OUT/licenses/lzma.txt"
cp recomp/engine/third_party/tomlplusplus/LICENSE "$OUT/licenses/tomlplusplus-MIT.txt"
cp fonts/NotoSans-OFL.txt "$OUT/licenses/NotoSans-OFL-1.1.txt"
echo "recomp: $OUT ($(du -sh "$OUT" | cut -f1))"
