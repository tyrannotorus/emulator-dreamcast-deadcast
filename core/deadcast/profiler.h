// Deadcast: a sampling profiler, on with DEADCAST_PROF=<file>. Each thread that registers is interrupted
// every millisecond of its own CPU time (SIGPROF from a per-thread CPU-time timer), and where it was
// is noted. profilerDump() writes the samples, with where the executable and the translated SH4 code
// are mapped, and for samples in translated code the SH4 block they ran.
// The translated blocks count their runs too, for where the SH4's own time goes (guest cycles).
#pragma once
#include "types.h"

void profilerStartThread(const char *name);
// Whether the translated blocks count their runs (RuntimeBlockInfo::runs).
bool profilerCountsRuns();
// a block gone (discarded, or the cache reset), having run that many times: the "runs" histogram
void profilerBlockGone(u64 runs);
// Call while the translated blocks still exist (before the emulator resets).
void profilerDump();
