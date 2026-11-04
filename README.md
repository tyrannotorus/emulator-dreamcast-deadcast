# Deadcast
A Sega Dreamcast emulator targeting optimal performance on the Trimui Brick/Hammer.

# Features
## Emulator
- Many games play well or close to full speed on the TrimUI Brick/Hammer.
- USB-C / Bluetooth device-to-device co-op play.
- In-app Recomp store for game mods

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
Deadcast hosts: it starts the module as a child process and sleeps until MENU, and the menu, the module's options and the store
stay Deadcast's.

### Layout

- `core/`: the emulator, a Flycast fork changed in place (nothing on the device says Flycast). Deadcast's
  own code is in `core/deadcast/`: the menu's emulator side, save states, the on-screen text (`osd.h`), the
  two-Brick link. Everything on the screen is drawn on the CPU by `ui/`.
- `ui/`: the menu library, a submodule: pages, the module host, the store, and the tg5040 toolchain.
- `store/`: the recomp modules the store lists (`registry.txt`). An Action rebuilds the list on the `store`
  branch, hourly if GitHub keeps to its schedule, so run it by hand after publishing a module.
- `shell/brick/`: build and package scripts. `pak.json`: the Pak Store's entry.

Only source, build scripts and READMEs go in git; never disc images or anything extracted from them.

### Build

On an x86-64 Linux host with clang, lld, CMake, Ninja, Docker (only to copy the toolchain image's
sysroot) and these packages installed: `curl`, `vulkan-headers`, `glslang`.

```
git submodule update --init ui core/deps/{asio,freetype,libchdr,tinygettext}
ui/toolchain/toolchain.sh                # the tg5040 sysroot, out of ghcr.io/loveretro/tg5040-toolchain
ui/toolchain/devlibs.sh SERIAL           # the Brick's libcurl.so.4, over adb
shell/brick/build.sh                     # build-brick/deadcast/deadcast
shell/brick/package.sh                   # build-brick/DC-DEADCAST.pak and DC-DEADCAST.pak.zip
```

### Recomp modules

- Each is its own repository, built on the
  [recomp SDK](https://github.com/tyrannotorus/emulator-dreamcast-deadcast-recomp-sdk): the rules, the
  folder and `module.cfg` are in its README.
- Listing one: add its repository to `store/registry.txt`. The store then offers its latest release.
- In the menu a module goes by its manifest's `title` (`Rez Recompiled`); in the docs it is the
  "<game title> recomp module". Never the game's title alone.
- A module's options, in Options with Core on it: Show FPS first, the module's settings after, then its own
  Controls if it declares any (nothing is shared with the emulator's). They are kept in its own
  `recomp-data/<id>/settings.cfg`, and go when it is deleted. Getting, updating and deleting a module
  are the Recomp Store's (on the main page).
- Any error of a module's shows the error with one button, `A TRY EMULATOR CORE`: Core becomes the emulator and the
  game starts again in it. The module stays installed, to switch back to in Options or to update in the store.

### Releasing

The version is `project(deadcast VERSION ...)` in `CMakeLists.txt` and `version` in `pak.json`; the
release's tag is the version, and its asset is `build-brick/DC-DEADCAST.pak.zip`. The Pak Store reads `pak.json`
from `main`, so the release must exist before the new version reaches `main`. Recomp modules have their own releases.

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

MENU opens Deadcast's menu (`ui/`, `core/deadcast/menu.cpp`):
Continue, Save and Load (ten slots, with pictures), Options, Recomp Store and Quit. Options holds Core and, under it, the options of the core picked: the emulator's Console, Video, Audio,
Controls and Local Multiplayer (each setting is saved as it changes), or the module's settings. Recomp Store lists
every recomp module: download, update, delete. Updating the module being played says it requires a restart, and A restarts the game in the new version. Going back from Options with another core picked asks to restart: Okay starts the game again in
it, Cancel puts the core back. Each core's options can be set from either core; Local Multiplayer, which plays in
the emulator, asks for that restart first. The menu shows whether the Brick is connected to a
network. FN2 saves a state, FN1 loads it and L2 takes a screenshot. SELECT swaps the d-pad between the
Dreamcast's d-pad (at first) and its analog stick, for games that only read the stick. Controls remaps
them all and the Dreamcast's buttons.
