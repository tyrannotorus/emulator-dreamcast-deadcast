#!/bin/sh
# Cross-build Deadcast (Flycast) for NextUI's tg5040 handhelds (TrimUI Brick, aarch64) into build-brick/deadcast.
# Needs:
#  - the toolchain (ui/toolchain/toolchain.sh), in TG5040_ROOT or build-brick/toolchain
#  - the device's libcurl.so.4 (ui/toolchain/devlibs.sh SERIAL), in TG5040_DEVLIBS or build-brick/devlibs
#  - the host's curl headers (curl); the sysroot has none
#  - the host's Vulkan headers (vulkan-headers; the sysroot's are too old) and glslangValidator
#    (glslang), which compiles the shaders to SPIR-V
#  - the submodules ui, asio, freetype, libchdr, rcheevos and tinygettext
# Vulkan only, to the screen through fb0 (core/rend/vulkan/panel.h); SDL2 (the device's) for the pad
# and the audio. Arguments go to ninja.
set -e
cd "$(dirname "$0")/../.."
B=build-brick/deadcast
TC=${TG5040_ROOT:-$PWD/build-brick/toolchain}
[ -d "$TC/aarch64-nextui-linux-gnu/libc" ] || { echo "brick: no toolchain (ui/toolchain/toolchain.sh)"; exit 2; }
CURLLIB=${TG5040_DEVLIBS:-$PWD/build-brick/devlibs}/libcurl.so.4
[ -f "$CURLLIB" ] || { echo "brick: no $CURLLIB (ui/toolchain/devlibs.sh SERIAL)"; exit 2; }
for dep in asio freetype libchdr rcheevos tinygettext; do
    [ -n "$(ls -A "core/deps/$dep" 2>/dev/null)" ] || { echo "brick: core/deps/$dep is empty (git submodule update --init core/deps/$dep)"; exit 2; }
done
mkdir -p build-brick/include
rm -rf build-brick/include/curl build-brick/include/vulkan build-brick/include/vk_video
cp -r /usr/include/curl /usr/include/vulkan /usr/include/vk_video build-brick/include/
# pkg-config sees the sysroot's packages only, never the host's
SYSROOT="$TC/aarch64-nextui-linux-gnu/libc"
export PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/pkgconfig:$SYSROOT/usr/share/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
unset PKG_CONFIG_PATH
mkdir -p "$B"
cmake -S . -B "$B" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$PWD/ui/toolchain/tg5040.cmake" \
    -DTG5040_ROOT="$TC" \
    -DTG5040_GC_SECTIONS=ON \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_SKIP_RPATH=ON \
    -DDEADCAST_VULKAN_INCLUDE="$PWD/build-brick/include" \
    -DUSE_HOST_SDL=ON \
    -DSDL2_DIR="$PWD/shell/brick/cmake" \
    -Dtinycmmc_DIR="$PWD/shell/brick/cmake/tinycmmc" \
    -DUSE_HOST_LIBZIP=OFF \
    -DFT_DISABLE_PNG=ON \
    -DWITH_SYSTEM_ZSTD=OFF \
    -DUSE_BREAKPAD=OFF \
    -DUSE_LUA=OFF \
    -DUSE_PULSEAUDIO=OFF \
    -DUSE_LIBAO=OFF \
    -DUSE_OPENMP=OFF \
    -DUSE_DREAMLINK=OFF \
    -DUSE_ICE=OFF \
    -DCURL_INCLUDE_DIR="$PWD/build-brick/include" \
    -DCURL_LIBRARY="$CURLLIB" > "$B.configure.log"
ninja -C "$B" "$@" deadcast
echo "brick: built $B/deadcast"
