# Recomp modules

A recomp module is a recompiled build of one Dreamcast game: the game's SH-4 code translated to C++
and compiled for the TrimUI Brick, played instead of Deadcast's emulation. Where a module exists for
the disc being played, Deadcast's menu offers it under **Core Settings**:

- **Current Core** picks Emulator or Recomp for that game. Changing it restarts the game in the
  other core.
- **Recomp Settings** shows the module's settings.
- **Download Recomp Module** (or **Check for Recomp Update** once installed) fetches the module.

Modules live in this repository. A module is added by a pull request and reviewed like any other
code. Deadcast downloads modules only from Deadcast's own releases, and only files whose SHA-256 the
Deadcast build already knows. It never downloads from anyone else's repository.

## What every module follows

1. **No in-game settings, and no in-game menu changes.** A module leaves the game's own menus
   exactly as the original game has them. Everything a module lets the player choose is a setting it
   declares in its manifest. Deadcast's Recomp Settings page shows and saves those settings, and the
   module reads them from Deadcast's store.
2. **Deadcast's menu, not one of its own.** MENU opens Deadcast's menu over the held game, through
   the shared port layer (`src/framework/deadcast.cpp`, built on `../frontend`). Save and Load (the
   emulator's save states) and Emulator Settings are shown disabled.
3. **No game data in git.** No disc images, nothing extracted from them, and no translated code.
   The translation is generated at build time from the builder's own disc (`<module>/game/extracted`
   is git-ignored).
4. **The disc is checked.** The manifest lists every disc the module plays, by the SHA-1 its CHD's
   header records. Deadcast offers the module only for those discs, and the engine checks the disc
   again before it runs.

## A module's folder

```
recomp/<id>/
  module.cfg          the manifest
  game/<id>.toml      the game: its discs, its boot file, relocations, hooks, functions
  game/CMakeLists.txt dreamcomp_add_port(<id> ...) with the module's own sources
  src/                native code replacing the game's libraries, and the module's hooks
```

The engine (`engine/`) and the port layer (`src/framework/`, `include/dreamcomp/`) are shared by every
module.

### module.cfg

```
id = rez                  the folder's name
name = Rez
version = 1.0.1
exec = rez                the executable, beside the manifest
disc = 9711a0d0...        each disc it plays: the SHA-1 its CHD's header records (one line each)
setting = scale | Resolution | 1=640x480, panel=1024x768 | 1
                          each setting: key | name | value=label, ... | default
```

The port layer applies `scale` (the resolution) itself, live. Any other setting a module declares,
it reads from Deadcast's store (`deadcast::recomp::get("<id>.<key>")`, `frontend/recomp.h`).

## How Deadcast runs a module

Deadcast keeps installed modules in `<DEADCAST_HOME>/recomp/<id>/`. When a game's core is Recomp,
Deadcast's first act is to `exec` the module, `<exec> <disc>`, so the emulator never starts. A module
keeps its own files (its settings file, memory card, caches) in `<DEADCAST_HOME>/recomp-data/<id>/`.
Choosing the other core in the menu ends the process with exit code 86, and Deadcast's `launch.sh`
starts the game again in the core now chosen.

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

Rez Recompiled (`rez/`) is the first module. Its engine came from the standalone Rez Recompiled
port, which owes much to
[dream-recomp](https://github.com/phobos665/dream-recomp), [dreamcomp](https://github.com/cherryduck/dreamcomp)
and [flycast](https://github.com/flyinghead/flycast).
