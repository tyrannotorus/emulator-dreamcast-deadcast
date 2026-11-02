# Recomp modules

A recomp module is a recompiled build of one Dreamcast game: the game's SH-4 code translated to C++
and compiled for the TrimUI Brick, played instead of Deadcast's emulation. Where a module exists for
the disc being played, Deadcast's menu offers it under **Options**:

- **Current Core** picks Emulator or Recomp for that game. Once another core is picked, the main
  page's **Continue** reads **Restart**, which starts the game again in it. Leaving the menu any
  other way puts the game's own core back.
- **Emulator Options** and **Recomp Options** hold each core's settings. Both can be set from either
  core: each core reads its settings when it starts.
- **Recomp Options** is greyed until the game's module is installed. It starts with **Show FPS**,
  followed by the module's own settings.

**Recomp Store**, on the main page, lists every recomp module, for any game: those installed, then those
available. A module's own page there downloads, updates or deletes it.

Modules live in this repository. A module is added by a pull request and reviewed like any other
code. Deadcast downloads modules only from Deadcast's own releases, and only files whose SHA-256 the
Deadcast build already knows. It never downloads from anyone else's repository.

## What every module follows

1. **No in-game settings, and no in-game menu changes.** A module leaves the game's own menus
   exactly as the original game has them. Everything a module lets the player choose is a setting it
   declares in its manifest. Deadcast's Recomp Options page shows and saves those settings, and the
   module reads them from Deadcast's store.
2. **Deadcast's menu, not one of its own.** MENU opens Deadcast's menu over the held game, through
   the shared port layer (`src/framework/deadcast.cpp`, built on `../frontend`). Save and Load (the
   emulator's save states) are shown disabled. Emulator Options' Console, Video and Audio settings
   are kept in the emulator's `emu.cfg` for its next start (`../frontend/options.h`); its Controls and
   Local Multiplayer are the emulator's own, shown disabled.
3. **No game data in git.** No disc images, nothing extracted from them, and no translated code.
   The translation is generated at build time from the builder's own disc (`<module>/game/extracted`
   is git-ignored).
4. **The disc is checked.** The manifest lists every disc the module plays, by the SHA-1 its CHD's
   header records. Deadcast offers the module only for those discs, and the engine checks the disc
   again before it runs.

## A module's folder

Every module has a folder of its own in `modules/`:

```
recomp/modules/<id>/
  README.md           the module, for players and builders (below)
  module.cfg          the manifest
  game/<id>.toml      the game: its discs, its boot file, relocations, hooks, functions
  game/CMakeLists.txt dreamcomp_add_port(<id> ...) with the module's own sources
  src/                native code replacing the game's libraries, and the module's hooks
```

The engine (`engine/`) and the port layer (`src/framework/`, `include/dreamcomp/`) are shared by every
module. Each module is added to the build in `CMakeLists.txt` (`add_subdirectory(modules/<id>/game)`).

### README.md

A module's README follows `modules/rez/README.md`, kept short: the game in a line or two, then these
sections, in this order:

- **Supported Formats:** every disc the module plays: region, format and the CHD header's SHA-1
  (as in `module.cfg`). Not the file name: a disc can be named anything.
- **Features:** what the module adds to the game, a line each: the settings it declares in
  `module.cfg` and anything built in (60 fps, a higher resolution, ...). Show FPS is every module's,
  so it is not listed.
- **Files**, **Building** and **Credits**.

### module.cfg

```
id = rez                  the folder's name
name = Rez
version = 1.0.1
exec = rez                the executable, beside the manifest
disc = 9711a0d0...        each disc it plays: the SHA-1 its CHD's header records (one line each)
title = Rez Recompiled    what Deadcast's menu calls it
setting = scale | Resolution | 1=640x480, panel=1024x768 | 1
                          each setting: key | name | value=label, ... | default
```

The port layer applies `scale` (the resolution) itself, live. Any other setting a module declares,
it reads from Deadcast's store (`deadcast::recomp::get("<id>.<key>")`, `frontend/recomp.h`).

Every module's Recomp Options starts with **Show FPS**, which a module does
not declare. Show FPS is the frame rate in the bottom-left corner, as Deadcast's emulator shows it
(`F:59.9`). The settings a module declares follow it, in the manifest's order. The port layer draws the counter the same way for every
module, copied into each frame as it is presented, so the frames keep the engine's fast path.

## How Deadcast runs a module

Deadcast keeps installed modules in `<DEADCAST_HOME>/recomp/<id>/`. When a game's core is Recomp,
Deadcast's first act is to `exec` the module, `<exec> <disc>`, so the emulator never starts. A module
keeps its own files (its settings file, memory card, caches) in `<DEADCAST_HOME>/recomp-data/<id>/`.
Restarting in the other core (Current Core, then Restart) ends the process with exit code 86, and
Deadcast's `launch.sh` starts the game again in the core now chosen.

A module keeps nothing outside those two folders: deleting it in the **Recomp Store** removes both, saves
included, and the store's keys for the module. Deleted from inside the module, the module only marks
itself (`recomp/<id>.delete`) and restarts the game emulated (exit code 86); Deadcast deletes it as it
starts, along with anything an install cut short left behind. Only a folder named as its manifest's
`id` counts as an installed module.

## Building and contributing

Build a module from your own disc:

```
shell/brick/toolchain.sh
shell/brick/devlibs.sh <adb serial>
shell/brick/recomp.sh <id> /path/to/your/disc.chd
```

The result is `build-brick/recomp/<id>/`: what Deadcast installs. A pull request carries the module's
folder (manifest, game config, sources) and any changes to the shared engine or port layer. It never
carries a binary or anything from the disc. Release binaries are built by the maintainer from the
reviewed source and the maintainer's own disc: a binary from a pull request cannot be checked without
building it again. With the same disc and the pinned toolchain, both builds come out the same.

The Rez recomp module (`modules/rez/`) is the first. Its engine came from the standalone Rez Recompiled
port, which owes much to
[dream-recomp](https://github.com/phobos665/dream-recomp), [dreamcomp](https://github.com/cherryduck/dreamcomp)
and [flycast](https://github.com/flyinghead/flycast).
