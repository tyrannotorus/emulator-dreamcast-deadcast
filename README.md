## Deadcast
A flycast-based, Sega Dreamcast emulator targeting optimal performance on the Trimui Brick/Hammer.

## Summary
This project will be an unattended AI incrementally improving the performance of dreamcast emulation
on the TrimUI Brick/Hammer.

## Features
- Many games play well, or close to full speed on the TrimUI Brick/Hammer with some exceptions.
- USB-C device-to-device co-op play with game transfer.

## AI Slop Section

Deadcast runs on the TrimUI Brick (NextUI, tg5040) only, as `Emus/tg5040/DC-Deadcast.pak`. It renders
with Vulkan on the Brick's PowerVR GE8300 straight to the panel through fb0; SDL2 reads the pad and
plays the audio; the SH4 runs on the ARM64 dynarec.

Information about configuration and supported features can be found on
[**TheArcadeStriker's flycast wiki**](https://github.com/TheArcadeStriker/flycast-wiki/wiki).

### Build

On an x86-64 Linux host with clang, lld, CMake, Ninja, Docker (only to copy the toolchain image's
sysroot) and these packages installed: `curl`, `vulkan-headers`, `glslang`.

```
shell/brick/toolchain.sh                 # the tg5040 sysroot, out of ghcr.io/loveretro/tg5040-toolchain
shell/brick/devlibs.sh SERIAL            # the Brick's libcurl.so.4, over adb
git submodule update --init core/deps/{asio,freetype,libchdr,rcheevos,tinygettext}
shell/brick/build.sh                     # build-brick/deadcast/deadcast
shell/brick/package.sh                   # build-brick/DC-Deadcast.pak
```

Copy `build-brick/DC-Deadcast.pak` to the SD card's `Emus/tg5040/`. Games go in `Roms/Dreamcast
(DC-Deadcast)/`: a disc (`.chd`, `.gdi`, `.cdi`) or an `.m3u` naming one elsewhere on the card. The
BIOS is read from `Bios/DC/` (`dc_boot.bin`; `naomi.zip` for NAOMI games).

### Controls

MENU opens Deadcast's menu, drawn like NextUI's own emulators' (`core/deadcast/menu.cpp`): Continue,
Save and Load (ten slots, with pictures), Options (Frontend, Emulator, Controls, Save Changes) and
Quit. FN2 saves a state, FN1 loads it and L2 takes a screenshot. SELECT swaps the d-pad between the
Dreamcast's d-pad (at first) and its analog stick, for games that only read the stick. Controls remaps
them all and the Dreamcast's buttons.
