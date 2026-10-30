// Deadcast: idle skipping for known wait loops. A game waiting for an interrupt (a vblank, say) in a
// loop of its own spends that time running the loop; the console's CPU would idle through it, and so
// can the emulator: time goes by as the loop would make it go, without running it. Each loop is
// recognized by its code, so only that game's loop is ever skipped, and only as long as nothing the
// loop looks at has changed.
#pragma once
#include "types.h"

struct RuntimeBlockInfo;

namespace deadcast
{
using WaitLoopHook = void (DYNACALL *)(u32 vaddr);
// If the block is part of a known wait loop, what its code calls first (with its address).
WaitLoopHook waitLoopHook(const RuntimeBlockInfo *block);
extern u32 waitLoopTurnsSkipped;	// for the perf line
extern u32 sleepTimeslices;		// the timeslices the SH4 spent asleep (sleep), for the perf line
}
