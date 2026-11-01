# Rez recomp module

Rez (United Game Artists, 2001), recompiled for the TrimUI Brick. Deadcast plays it in place of its
emulator when the game's Current Core is Recomp.

## Supported Formats

| Region | Format | SHA-1 |
|---|---|---|
| Japan | CHD | `9711a0d0c21af3685672b9a42c72abf999fc7150` |
| Europe | CHD | `01b7c1cc29a840b141a4ae713037eb1224fac233` |

The SHA-1 is the CHD header's, shown by `chdman info` as **SHA1**. Other images, including GDI, CDI
and BIN/CUE, are refused.

## Features

- **60 fps:** the stages are drawn at 60 frames a second, where the original draws them at 30.
- **Resolution:** 640x480 or 1024x768 (the Brick's screen), set in Recomp Options.

## Files

```
module.cfg              the manifest
game/rez.toml           the game: its discs, boot file, relocations, hooks, functions
game/rez-pipelines.txt  the renderer's pipelines Rez uses
game/CMakeLists.txt     the module's build
src/                    native code replacing the game's libraries, and the port's hooks (rez.cpp)
```

## Building

From your own disc:

```
shell/brick/recomp.sh rez "/path/to/Rez (Europe) (En,Ja,Fr,De,Es,It).chd"
```

See `../../README.md` for the toolchain.

## Credits

Built from the standalone Rez Recompiled port, which owes much to
[dream-recomp](https://github.com/phobos665/dream-recomp), [dreamcomp](https://github.com/cherryduck/dreamcomp)
and [flycast](https://github.com/flyinghead/flycast).
