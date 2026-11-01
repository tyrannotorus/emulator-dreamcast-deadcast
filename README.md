# Deadcast
A Sega Dreamcast emulator (flycast-based) targeting optimal performance on the Trimui Brick/Hammer.

# Emulator Features
- Many games play well or close to full speed on the TrimUI Brick/Hammer.
- USB-C / Bluetooth device-to-device co-op play.

# Recomp Features
- Optional downloadable recomp modules for game mods.

# Supported Devices
## Trimui Brick/Hammer
1. Install Deadcast via the NextUI Pak Store
2. Put Dreamcast roms in a `Roms/Dreamcast (DC-DEADCAST)/` folder in the root of your SD Card.
3. Put Dreamcast bios (naomi.zip) in a `Emus/tg5040/DC-DEADCAST.pak/bios/` folder in the root of your SD Card.

## AI Slop Section

Deadcast runs on the TrimUI Brick (NextUI, tg5040) only, as `Emus/tg5040/DC-DEADCAST.pak`. It renders
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
shell/brick/package.sh                   # build-brick/DC-DEADCAST.pak
```

### Install

Copy `build-brick/DC-DEADCAST.pak` to the SD card's `Emus/tg5040/`.

NextUI picks a game's emulator by the tag in parentheses at the end of its Roms folder's name: a game
in `Roms/<name> (TAG)/` runs with `Emus/tg5040/TAG.pak`. Deadcast's tag is `DC-DEADCAST`, because `DC`
is taken by the Pak Store's Flycast pak (`DC.pak`); the two can be installed side by side.

Put games in a folder such as `Roms/Dreamcast (DC-DEADCAST)/`: a disc (`.chd`, `.gdi`, `.cdi`), or an
`.m3u` naming a disc elsewhere on the card (to share discs with another pak without a second copy).
NextUI shows folders without their tag and merges console folders whose names match up to the tag, so
give Deadcast's folder a different name than another Dreamcast pak's (`Sega Dreamcast (DC)` for
`DC.pak`). `Roms/map.txt` renames folders in the menu, one per line: the folder's name, a tab, the
name to show.

The BIOS goes in the pak's own `bios/` folder (`Emus/tg5040/DC-DEADCAST.pak/bios/`): `dc_boot.bin` for
Dreamcast discs, `naomi.zip` for NAOMI games. Deadcast looks there first, then in `Bios/DC/`, where
other Dreamcast paks keep theirs; if neither has it, it shows which file to copy where. A Pak Store
update keeps the pak's `bios/`; uninstalling the pak removes it. Settings, VMUs and save states are
kept in `.userdata/tg5040/DC-DEADCAST/`.

### Controls

MENU opens Deadcast's menu, drawn like NextUI's own emulators' (`frontend/`, `core/deadcast/menu.cpp`):
Continue, Save and Load (ten slots, with pictures), Options and Quit. Options holds Current
Core, Emulator Options (Console, Video, Audio, Controls, Local Multiplayer; each setting is saved as it
changes), Recomp Options and Download Recomp Module. The menu shows whether the Brick is connected to a
network. FN2 saves a state, FN1 loads it and L2 takes a screenshot. SELECT swaps the d-pad between the
Dreamcast's d-pad (at first) and its analog stick, for games that only read the stick. Controls remaps
them all and the Dreamcast's buttons.

### Recomp modules

Some games can be played as a recompiled build instead of emulated: a recomp module, chosen per game in
Options. Modules live in `recomp/` (Rez Recompiled is the first); `recomp/README.md` describes
what a module is, how one is built from its builder's own disc, and the rules every module follows.
