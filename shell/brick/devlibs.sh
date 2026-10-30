#!/bin/sh
# Libraries the Brick has and the toolchain's sysroot has not, pulled over adb into
# build-brick/devlibs so the Brick build can link against them. Never shipped: the device has them.
#  - libcurl.so.4 (/usr/lib); its headers come from the host (build.sh)
#   shell/brick/devlibs.sh SERIAL        (adb devices -l)
set -e
cd "$(dirname "$0")/../.."
SERIAL=${1:?usage: shell/brick/devlibs.sh SERIAL}
mkdir -p build-brick/devlibs
adb -s "$SERIAL" pull /usr/lib/libcurl.so.4 build-brick/devlibs/libcurl.so.4 > /dev/null
echo "devlibs: build-brick/devlibs/libcurl.so.4 ($(wc -c < build-brick/devlibs/libcurl.so.4) bytes)"
