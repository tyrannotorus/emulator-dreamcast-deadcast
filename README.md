# Deadcast
A Sega Dreamcast emulator targeting optimal performance on the Trimui Brick/Hammer.

# Features
## Emulator
- Many games play well or close to full speed on the TrimUI Brick/Hammer.
- USB-C / Bluetooth device-to-device co-op play.
## Recomps
- Optional downloadable recomp modules for game mods.

# Supported Devices
## Trimui Brick/Hammer
1. Install Deadcast via the [NextUI](https://github.com/LoveRetro/NextUI) [Pak Store](https://github.com/LoveRetro/nextui-pak-store).
2. Put Dreamcast roms in a `/Roms/Dreamcast (DC-DEADCAST)/` folder on your SD Card.
3. Put the Dreamcast BIOS (`dc_boot.bin`) in the `/Emus/tg5040/DC-DEADCAST.pak/bios/` folder on your SD Card.

# Credits
- [Flycast](https://github.com/flyinghead/flycast): the emulator Deadcast is built from.
- [dreamcomp](https://github.com/cherryduck/dreamcomp): the Dreamcast recompiler the recomp engine builds on.
- [Rez Recompiled](https://github.com/tyrannotorus/recomp-dreamcast-rez): the port the Rez recomp module comes from.

---

## AI Section

Notes for agents working on this repo. Everything above the line is the maintainer's: leave it as written.

Deadcast runs on the TrimUI Brick (NextUI, tg5040) only, as `Emus/tg5040/DC-DEADCAST.pak`. It renders
with Vulkan on the Brick's PowerVR GE8300 straight to the panel through fb0; SDL2 reads the pad and
plays the audio; the SH4 runs on the ARM64 dynarec. A recomp module is a separate executable that
Deadcast execs in its place.

### Layout

- `core/`: the emulator, a Flycast fork changed in place (nothing on the device says Flycast). Deadcast's
  own code is in `core/deadcast/`: the menu's emulator side, save states, the two-Brick link.
- `frontend/`: the menu library the emulator and every recomp module share: pages, Options, Emulator
  Options' settings (`options.h`), the module store, and the catalog of downloadable modules
  (`catalog.h`, each file by SHA-256).
- `recomp/`: the recomp engine, the port layer modules share, and the modules (`recomp/modules/<id>/`).
- `shell/brick/`: build, package and release scripts. `pak.json`: the Pak Store's entry.

Only source, build scripts and READMEs go in git; never disc images or anything extracted from them.

### Build

On an x86-64 Linux host with clang, lld, CMake, Ninja, Docker (only to copy the toolchain image's
sysroot) and these packages installed: `curl`, `vulkan-headers`, `glslang`.

```
shell/brick/toolchain.sh                 # the tg5040 sysroot, out of ghcr.io/loveretro/tg5040-toolchain
shell/brick/devlibs.sh SERIAL            # the Brick's libcurl.so.4, over adb
git submodule update --init core/deps/{asio,freetype,libchdr,rcheevos,tinygettext}
shell/brick/build.sh                     # build-brick/deadcast/deadcast
shell/brick/package.sh                   # build-brick/DC-DEADCAST.pak and DC-DEADCAST.pak.zip
```

### Recomp modules

- Adding one: `recomp/README.md` (the rules, the folder, `module.cfg`), with `recomp/modules/rez/` as
  the example. Its README follows `recomp/modules/rez/README.md`.
- Building one: `shell/brick/recomp.sh <id> <disc.chd>` makes `build-brick/recomp/<id>/`.
- Releasing one: `shell/brick/recomp-release.sh <id> <version>` packs it and writes its size and
  SHA-256 into `frontend/catalog.h`; build Deadcast after. Any change to the module's binary changes
  its SHA-256, so pack it again before every release.
- In the menu a module goes by its manifest's `title` (`Rez Recompiled`); in the docs it is the
  "<game title> recomp module". Never the game's title alone.
- Recomp Options: Show FPS first, the module's settings after. Getting, updating and deleting a module
  are the Recomp Store's (on the main page).

### Releasing

The version is `project(deadcast VERSION ...)` in `CMakeLists.txt` and `version` in `pak.json`; the
release's tag is the version. Its assets are `build-brick/DC-DEADCAST.pak.zip` and each
`build-brick/release/recomp-<id>-<version>.tar.gz`, which Deadcast downloads from
`releases/download/<version>/`.

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
Continue, Save and Load (ten slots, with pictures), Options, Recomp Store and Quit. Options holds Core,
Emulator Options (Console, Video, Audio, Controls, Local Multiplayer; each setting is saved as it
changes) and Recomp Options (the settings of the game's module, once it is installed). Recomp Store lists
every recomp module: download, update, delete. Picking the other core turns Continue into Restart, which starts the game again in
it; each core's options can be set from either core. The menu shows whether the Brick is connected to a
network. FN2 saves a state, FN1 loads it and L2 takes a screenshot. SELECT swaps the d-pad between the
Dreamcast's d-pad (at first) and its analog stick, for games that only read the stick. Controls remaps
them all and the Dreamcast's buttons.
