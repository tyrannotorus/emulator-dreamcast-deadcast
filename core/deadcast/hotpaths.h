// Deadcast: the guest's hottest routines run as native code. A loop of a game's own, recognized by its code (anywhere
// in main RAM: its branches are relative), is run by a function written for it in place of the dynarec's blocks,
// which split it into blocks of 2 to 6 instructions, each with its own entry and exit costs. The function leaves the
// registers, memory and T as the blocks would, charges the cycles block by block as they would (dec_blockCycles),
// and stops at the start of the block where the cycles have run out, as they stop there for the scheduler; the
// dynarec's blocks go on from there. Without the MMU only.
#pragma once
#include "types.h"

struct RuntimeBlockInfo;

namespace deadcast
{
// called from the block's start, its own cycles charged; returns the next pc, or 0, nothing done, for the block's own code
using HotPath = u32 (DYNACALL *)(u32 vaddr);
// the native code for the block's loop, if it starts one this knows
HotPath hotPath(const RuntimeBlockInfo *block);
}
