// Deadcast: the SH4 blocks a game ran, kept between runs in <disc>.blocks beside its save states. Loading a
// save state empties the code cache, and the game's first seconds then go to compiling its blocks again
// (thousands a second). Instead, right after the state is loaded, each block of the list whose SH4 code in
// RAM is still byte for byte the same is compiled at once, before the game goes on. Nothing else changes:
// the same blocks, compiled earlier. DEADCAST_CODECACHE=0 leaves them to be compiled as they run (for
// comparisons); the list is kept either way.
#pragma once

namespace deadcast::codecache
{

// the blocks compiled now, written for this disc (as a game ends)
void save();
// after a save state is loaded: the recorded blocks compiled
void warm();

}
