# Rez

Rez (United Game Artists, 2001), recompiled for the TrimUI Brick: the game's SH-4 code translated to
C++ and built for the Brick, played instead of Deadcast's emulation when a Rez disc's Current Core is
Recomp. It is Deadcast's first recomp module, built from the standalone Rez Recompiled port.

Module `rez`, version 1.0.1.

## Supported Formats

Redump's CHD images of the two Dreamcast releases, version 1.003. The module is offered only for these
two discs, and the game checks the disc again before it runs.

| Disc | Release | Format | SHA-1 |
|---|---|---|---|
| `Rez (Japan) (En,Ja,Fr,De,Es,It).chd` | Japan, HDR-0178 | CHD | `9711a0d0c21af3685672b9a42c72abf999fc7150` |
| `Rez (Europe) (En,Ja,Fr,De,Es,It).chd` | Europe, MK-5119250 | CHD | `01b7c1cc29a840b141a4ae713037eb1224fac233` |

The SHA-1 is the one the CHD's header records, which `chdman info` prints as **SHA1** (not
**Data SHA1**). Any other image is refused, including a GDI, CDI or BIN/CUE of the same disc. Rez had
no North American Dreamcast release.

## Recomp Features

What the module adds to the game, set in Deadcast's **Options > Recomp Options**. The game's own menus
are left as the original has them.

- **Resolution:** 640x480, the Dreamcast's own, or 1024x768, the Brick's screen. It changes at once,
  while playing.

## Playing

- **Speed:** full speed, 60 frames a second, at either resolution.
- **Saves:** the game saves to a memory card of its own, kept in
  `<DEADCAST_HOME>/recomp-data/rez/vmu_a1.bin`. It is not the emulator's memory card, so saves made in
  one core are not seen in the other. Save states are the emulator's only, and are disabled here.
- **First start:** the renderer's pipelines are made before the game starts (`game/rez-pipelines.txt`),
  so none is compiled mid-game.

## Files

```
module.cfg              the manifest: the discs above by SHA-1, the Resolution setting
game/rez.toml           the game: its discs, its boot file, relocations, hooks, functions
game/rez-pipelines.txt  the renderer's pipelines Rez uses
game/CMakeLists.txt     the module's build
src/                    native code replacing the game's libraries (graphics, sound, file system,
                        peripherals), and the port's settings and hooks (rez.cpp)
```

## Building

From your own disc, one of the two above:

```
shell/brick/recomp.sh rez "/path/to/Rez (Europe) (En,Ja,Fr,De,Es,It).chd"
```

The result is `build-brick/recomp/rez/`. `../../README.md` covers the toolchain and contributing.

## Credits

The engine and the port came from the standalone Rez Recompiled port, which owes much to
[dream-recomp](https://github.com/phobos665/dream-recomp), [dreamcomp](https://github.com/cherryduck/dreamcomp)
and [flycast](https://github.com/flyinghead/flycast).
