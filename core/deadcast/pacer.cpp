#include "pacer.h"
#include "types.h"
#include "audio/audiostream.h"
#include "cfg/option.h"
#include "hw/sh4/sh4_sched.h"
#include "hw/pvr/spg.h"
#include "network/ggpo.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace deadcast::pacer
{
namespace
{

// an experiment for now: on with DEADCAST_PACER=1 (it evened the emulated frames, but did not change the screen)
const bool enabled = std::getenv("DEADCAST_PACER") != nullptr && std::strcmp(std::getenv("DEADCAST_PACER"), "0") != 0;
bool started;
u64 lastCycles;
s64 target;			// ns, CLOCK_MONOTONIC: when the SH4's time reaches real time
double rate = 1.0;	// real ns per emulated ns

s64 nowNs()
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (s64)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

}	// namespace

void vblank()
{
	if (!enabled || !config::LimitFPS || settings.input.fastForwardMode || ggpo::active())
	{
		started = false;
		return;
	}
	const u64 cycles = sh4_sched_now64();
	const s64 now = nowNs();
	if (!started)
	{
		started = true;
		lastCycles = cycles;
		target = now;
		return;
	}
	const u64 emulated = cycles - lastCycles;
	lastCycles = cycles;
	// half full is the aim for the audio output's buffer
	const float fill = audioFill();
	if (fill >= 0.f)
	{
		const double want = 1.0 + std::clamp((fill - 0.5) * 0.02, -0.005, 0.005);
		rate += (want - rate) * 0.05;
	}
	target += (s64)((double)emulated * (1e9 / SH4_MAIN_CLOCK) * rate);
	// far behind (a pause, a load, a heavy stretch): from now, no rush to make it up
	if (now > target + 50000000)
	{
		target = now;
		return;
	}
	if (target <= now)
		return;
	perfTrace('z');
	timespec until { (time_t)(target / 1000000000), (long)(target % 1000000000) };
	while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &until, nullptr) == EINTR)
		;
	perfTrace('Z');
}

}
