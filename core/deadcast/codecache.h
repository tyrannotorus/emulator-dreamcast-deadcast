// Deadcast: the SH4 blocks a game ran, kept between runs in <disc>.blocks beside its save states. Loading a
// save state empties the code cache; right after it, each listed block whose SH4 code in RAM is still byte
// for byte the same is compiled at once, rather than in the game's first seconds. The same blocks, earlier.
#pragma once

namespace deadcast::codecache
{

// the blocks compiled now, written for this disc (as a game ends)
void save();
// after a save state is loaded: the recorded blocks compiled
void warm();

}
