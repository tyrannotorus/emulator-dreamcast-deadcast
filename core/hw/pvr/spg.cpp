#include <vector>
#include <string>
#include "spg.h"
#include "hw/holly/holly_intc.h"
#include "hw/holly/sb.h"
#include "hw/sh4/sh4_sched.h"
#include "hw/maple/maple_if.h"
#include "serialize.h"
#include "network/ggpo.h"
#include "hw/pvr/Renderer_if.h"
#include "ui/gui.h"
#include "hw/sh4/modules/mmu.h"
#include "hw/mem/addrspace.h"
#include "deadcast/waitloops.h"
extern u32 bmStatCompiled, bmStatCacheResets;
extern u64 bmStatCompileUs;

namespace aica { extern u32 aicaStatReads, aicaStatWrites, aicaStatMeets; extern u64 aicaStatWaitUs; void aicaWriteLogDump(); }
#include <string>
#include "stdclass.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <pthread.h>
#include <time.h>

#ifdef TEST_AUTOMATION
#include "input/gamepad_device.h"
#endif

//SPG emulation; Scanline/Raster beam registers & interrupts

static u32 clc_pvr_scanline;
static u32 pvr_numscanlines = 512;
static u32 prv_cur_scanline = -1;

#if !defined(NDEBUG) || defined(DEBUGFAST)
static u32 vblk_cnt;
static u64 last_fps;
#endif

// 27 mhz pixel clock
constexpr int PIXEL_CLOCK = 27 * 1000 * 1000;
static u32 Line_Cycles;
static u32 Frame_Cycles;
int render_end_schid;
int vblank_schid;

static std::array<double, 4> real_times;
static std::array<u64, 4> cpu_cycles;
static u32 cpu_time_idx;
bool SH4FastEnough;
u32 fskip;
// Deadcast: how far emulation is behind real time, in ms (spg_vblank, QueueRender)
float emulationLagMs;
// Deadcast: the times the SH4 was held back, ahead of the audio (AicaUpdate)
u32 sh4HeldBack;
float sh4WaitedMs;

// Deadcast: DEADCAST_PERF prints a line every second: vblanks against the game's rate, frames rendered
// and skipped, frames presented, and the longest gap between two of them.
static const bool perfEnabled = std::getenv("DEADCAST_PERF") != nullptr;
// Deadcast: and with DEADCAST_SHOTS=<folder>, the frame as rendered every 5 s, in <folder>/shot-<N>s.ppm
static const char *const perfShots = std::getenv("DEADCAST_SHOTS");
// Deadcast: DEADCAST_DUMP_RAM=<file> writes main RAM there once, 20 s in (or DEADCAST_DUMP_RAM_AT seconds)
static const char *const perfDumpRam = std::getenv("DEADCAST_DUMP_RAM");
static const u64 perfDumpRamAt = (std::getenv("DEADCAST_DUMP_RAM_AT") != nullptr ? atoi(std::getenv("DEADCAST_DUMP_RAM_AT")) : 20) * 1000000ull;
// Deadcast: with DEADCAST_DUMP_POLYS too, the polygons of the frame after each shot go to
// <folder>/polys-<N>s.txt (Renderer_if.cpp)
static const bool perfDumpPolys = std::getenv("DEADCAST_DUMP_POLYS") != nullptr;
// DEADCAST_SHOTS_EVERY=<seconds>: a shot that often instead of every 5 s
static const u32 perfShotsEvery = std::max(1, std::getenv("DEADCAST_SHOTS_EVERY") != nullptr ? atoi(std::getenv("DEADCAST_SHOTS_EVERY")) : 5);
static std::atomic<u32> perfPresentedFrames;
static std::atomic<u32> perfLongestPresentUs;
static std::atomic<u64> perfWaitUs;
static std::atomic<u64> perfRenderUs;
static std::atomic<u32> perfRenders, perfDraws, perfVertices, perfTextureRenders;
static std::atomic<u32> perfTextureUpdates, perfTextureBytes;
std::atomic<u32> audioUnderruns;
unsigned perfDrawCalls;
static std::atomic<u64> perfGpuUs[2], perfGpuMaxUs[2], perfBlockedUs[3], perfProcessUs, perfRenderMaxUs;
static std::atomic<u32> perfGpuCount[2];
static std::atomic<u32> perfLateGaps, perfShownFrames, perfShownRepeats, perfShownLongestUs;
static std::atomic<bool> perfRenderClockSet;
static clockid_t perfRenderClock;
static std::atomic<bool> perfAicaClockSet;
static clockid_t perfAicaClock;

void perfAicaThread()
{
	if (!perfEnabled)
		return;
	pthread_getcpuclockid(pthread_self(), &perfAicaClock);
	perfAicaClockSet.store(true, std::memory_order_release);
}

static u64 perfNowUs()
{
	using namespace std::chrono;
	return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

unsigned long long perfNow()
{
	return perfEnabled ? perfNowUs() : 0;
}

static void perfMax(std::atomic<u64>& max, u64 value)
{
	u64 now = max.load(std::memory_order_relaxed);
	while (value > now && !max.compare_exchange_weak(now, value, std::memory_order_relaxed))
		;
}

bool perfOn()
{
	return perfEnabled;
}

static const bool perfTraceOn = perfEnabled && std::getenv("DEADCAST_TRACE") != nullptr;
struct PerfTraceEvent
{
	u64 us;
	char what;
};
static std::vector<PerfTraceEvent> perfTraceEvents(perfTraceOn ? 1 << 20 : 0);
static std::atomic<u32> perfTraceCount;

void perfTrace(char what)
{
	if (!perfTraceOn)
		return;
	const u32 i = perfTraceCount.fetch_add(1, std::memory_order_relaxed);
	if (i < perfTraceEvents.size())
		perfTraceEvents[i] = { perfNowUs(), what };
}

void perfTraceDump()
{
	aica::aicaWriteLogDump();
	if (!perfTraceOn)
		return;
	const std::string path = std::string(perfShots != nullptr ? perfShots : "/tmp") + "/trace.txt";
	if (FILE *f = fopen(path.c_str(), "w"))
	{
		const u32 n = std::min<u32>(perfTraceCount.load(), (u32)perfTraceEvents.size());
		for (u32 i = 0; i < n; i++)
			fprintf(f, "%llu %c\n", (unsigned long long)perfTraceEvents[i].us, perfTraceEvents[i].what);
		fclose(f);
	}
}

void perfShown()
{
	if (!perfEnabled)
		return;
	static u64 last;
	const u64 now = perfNowUs();
	if (last != 0)
	{
		const u32 gap = (u32)(now - last);
		// a vblank or more went by on the old frame
		if (gap > 25000)
			perfShownRepeats.fetch_add(1, std::memory_order_relaxed);
		u32 longest = perfShownLongestUs.load(std::memory_order_relaxed);
		while (gap > longest && !perfShownLongestUs.compare_exchange_weak(longest, gap, std::memory_order_relaxed))
			;
	}
	last = now;
	perfTrace('V');
	perfShownFrames.fetch_add(1, std::memory_order_relaxed);
}

void perfGpu(int kind, unsigned long long us)
{
	if (!perfEnabled || kind < 0 || kind > 1)
		return;
	perfGpuUs[kind].fetch_add(us, std::memory_order_relaxed);
	perfGpuCount[kind].fetch_add(1, std::memory_order_relaxed);
	perfMax(perfGpuMaxUs[kind], us);
}

void perfRenderBlocked(int kind, unsigned long long start)
{
	if (perfEnabled && kind >= 0 && kind <= 2)
		perfBlockedUs[kind].fetch_add(perfNowUs() - start, std::memory_order_relaxed);
}

void perfProcessed(unsigned long long start)
{
	if (perfEnabled)
		perfProcessUs.fetch_add(perfNowUs() - start, std::memory_order_relaxed);
}

void perfRendered(unsigned long long start, unsigned draws, unsigned vertices, bool toTexture)
{
	if (!perfEnabled)
		return;
	if (toTexture)
		perfTextureRenders.fetch_add(1, std::memory_order_relaxed);
	const u64 took = perfNowUs() - start;
	perfRenderUs.fetch_add(took, std::memory_order_relaxed);
	perfMax(perfRenderMaxUs, took);
	perfRenders.fetch_add(1, std::memory_order_relaxed);
	perfDraws.fetch_add(draws, std::memory_order_relaxed);
	perfVertices.fetch_add(vertices, std::memory_order_relaxed);
}

void perfTextureUpdated(u32 size)
{
	if (!perfEnabled)
		return;
	perfTextureUpdates.fetch_add(1, std::memory_order_relaxed);
	perfTextureBytes.fetch_add(size, std::memory_order_relaxed);
}

void perfWaited(unsigned long long start)
{
	if (perfEnabled)
		perfWaitUs.fetch_add(perfNowUs() - start, std::memory_order_relaxed);
}

// CPU time of a thread's clock, in microseconds
static u64 perfCpuUs(clockid_t clock)
{
	timespec ts;
	if (clock_gettime(clock, &ts) != 0)
		return 0;
	return (u64)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

void perfPresented()
{
	if (!perfEnabled)
		return;
	if (!perfRenderClockSet.load(std::memory_order_acquire))
	{
		pthread_getcpuclockid(pthread_self(), &perfRenderClock);
		perfRenderClockSet.store(true, std::memory_order_release);
	}
	static u64 last;
	u64 now = perfNowUs();
	if (last != 0)
	{
		u32 gap = (u32)(now - last);
		if (gap > 25000)
			perfLateGaps.fetch_add(1, std::memory_order_relaxed);
		u32 longest = perfLongestPresentUs.load(std::memory_order_relaxed);
		while (gap > longest && !perfLongestPresentUs.compare_exchange_weak(longest, gap, std::memory_order_relaxed))
			;
	}
	last = now;
	perfPresentedFrames.fetch_add(1, std::memory_order_relaxed);
}

static void perfVblank()
{
	static u64 start, windowStart, emuCpu, renderCpu, aicaCpu;
	static u32 vblanks, frames, skipped;
	// the wall time between two emulated vblanks: over 17.5 ms (a frame is 16.68), that frame was late
	static u64 lastVblank, frameMaxUs;
	static u32 framesOver;
	u64 now = perfNowUs();
	if (lastVblank != 0)
	{
		const u64 frame = now - lastVblank;
		frameMaxUs = std::max(frameMaxUs, frame);
		if (frame > 17500)
			framesOver++;
	}
	lastVblank = now;
	perfTrace('v');
	bool renderClock = perfRenderClockSet.load(std::memory_order_acquire);
	if (start == 0)
	{
		start = windowStart = now;
		frames = FrameCount;
		skipped = fskip;
		perfPresentedFrames = 0;
		perfLongestPresentUs = 0;
		perfWaitUs = 0;
		emuCpu = perfCpuUs(CLOCK_THREAD_CPUTIME_ID);
		renderCpu = renderClock ? perfCpuUs(perfRenderClock) : 0;
		aicaCpu = perfAicaClockSet.load(std::memory_order_acquire) ? perfCpuUs(perfAicaClock) : 0;
		return;
	}
	vblanks++;
	if (now - windowStart < 1000000)
		return;
	double secs = (now - windowStart) / 1e6;
	double nominal = (double)SH4_MAIN_CLOCK / Frame_Cycles;
	u32 presented = perfPresentedFrames.exchange(0);
	u32 longest = perfLongestPresentUs.exchange(0);
	u64 waited = perfWaitUs.exchange(0);
	u32 renders = perfRenders.exchange(0);
	u64 renderUs = perfRenderUs.exchange(0);
	u32 draws = perfDraws.exchange(0);
	u32 vertices = perfVertices.exchange(0);
	u64 emuNow = perfCpuUs(CLOCK_THREAD_CPUTIME_ID);
	u64 renderNow = renderClock ? perfCpuUs(perfRenderClock) : 0;
	const bool aicaClock = perfAicaClockSet.load(std::memory_order_acquire);
	const u64 aicaNow = aicaClock ? perfCpuUs(perfAicaClock) : 0;
	double window = (double)(now - windowStart);
	const u32 gpuScene = perfGpuCount[0].exchange(0), gpuPage = perfGpuCount[1].exchange(0);
	const u64 gpuSceneUs = perfGpuUs[0].exchange(0), gpuPageUs = perfGpuUs[1].exchange(0);
	printf("Deadcast perf %6.1f s: vblank %5.2f/s %5.1f%% | rendered %5.2f/s skipped %5.2f/s | presented %5.2f/s longest %5.1f ms"
			" | emu cpu %3.0f%% waits %3.0f%% | render cpu %3.0f%% | mmu flushes %u+%u misses %u"
			" | renders %u (to textures %u) %.1f ms draws %u vertices %u | textures updated %u (%u KB) | audio underruns %u | aica reads %u writes %u waits %u %.1f%% | wait turns skipped %u | asleep %.1f%% | compiled %u in %.1f ms resets %u"
			" | gpu scene %.2f ms max %.2f page %.2f ms max %.2f | render max %.1f ms process %.1f ms blocked gpu %.1f%% page %.1f%% parse %.1f%% | emu frames late %u max %.1f ms | gaps over 25 ms %u | shown %.2f/s repeats %u longest %.1f ms | aica cpu %.0f%%\n",
			(now - start) / 1e6, vblanks / secs, vblanks / secs / nominal * 100.0,
			(FrameCount - frames) / secs, (fskip - skipped) / secs,
			presented / secs, longest / 1000.0,
			(emuNow - emuCpu) / window * 100.0, waited / window * 100.0,
			renderCpu != 0 ? (renderNow - renderCpu) / window * 100.0 : 0.0,
			mmuStatFullFlushes, mmuStatPartialFlushes, mmuStatMisses,
			renders, perfTextureRenders.exchange(0), renders ? renderUs / 1000.0 / renders : 0.0, renders ? draws / renders : 0, renders ? vertices / renders : 0,
			perfTextureUpdates.exchange(0), perfTextureBytes.exchange(0) / 1024,
			audioUnderruns.exchange(0),
			aica::aicaStatReads, aica::aicaStatWrites, aica::aicaStatMeets, aica::aicaStatWaitUs / window * 100.0,
			deadcast::waitLoopTurnsSkipped,
			deadcast::sleepTimeslices * (double)SH4_TIMESLICE / (vblanks / secs * Frame_Cycles * secs) * 100.0,
			bmStatCompiled, bmStatCompileUs / 1000.0, bmStatCacheResets,
			gpuScene ? gpuSceneUs / 1000.0 / gpuScene : 0.0, perfGpuMaxUs[0].exchange(0) / 1000.0,
			gpuPage ? gpuPageUs / 1000.0 / gpuPage : 0.0, perfGpuMaxUs[1].exchange(0) / 1000.0,
			perfRenderMaxUs.exchange(0) / 1000.0, perfProcessUs.exchange(0) / 1000.0 / std::max(renders, 1u),
			perfBlockedUs[0].exchange(0) / window * 100.0, perfBlockedUs[1].exchange(0) / window * 100.0,
			perfBlockedUs[2].exchange(0) / window * 100.0,
			framesOver, frameMaxUs / 1000.0, perfLateGaps.exchange(0),
			perfShownFrames.exchange(0) / secs, perfShownRepeats.exchange(0), perfShownLongestUs.exchange(0) / 1000.0,
			aicaClock && aicaCpu != 0 ? (aicaNow - aicaCpu) / window * 100.0 : 0.0);
	aicaCpu = aicaNow;
	framesOver = 0;
	frameMaxUs = 0;
	bmStatCompiled = bmStatCacheResets = 0;
	bmStatCompileUs = 0;
	deadcast::waitLoopTurnsSkipped = 0;
	deadcast::sleepTimeslices = 0;
	aica::aicaStatReads = aica::aicaStatWrites = aica::aicaStatMeets = 0;
	aica::aicaStatWaitUs = 0;
	mmuStatFullFlushes = mmuStatPartialFlushes = mmuStatMisses = 0;
	fflush(stdout);
	if (perfDumpRam != nullptr)
	{
		static bool dumped;
		if (!dumped && now - start >= perfDumpRamAt)
		{
			dumped = true;
			if (FILE *f = fopen(perfDumpRam, "wb"))
			{
				for (u32 a = 0; a < RAM_SIZE; a += 4)
				{
					u32 v = addrspace::read32(0x0C000000 + a);
					fwrite(&v, 4, 1, f);
				}
				fclose(f);
			}
		}
	}
	if (perfShots != nullptr)
	{
		static u32 lastShot;
		u32 t = (u32)((now - start) / 1000000);
		if (t / perfShotsEvery != lastShot / perfShotsEvery && t > 0)
		{
			gui_saveFrame(std::string(perfShots) + "/shot-" + std::to_string(t / perfShotsEvery * perfShotsEvery) + "s.ppm");
			if (perfDumpPolys)
				rend_dumpPolysAt(t / perfShotsEvery * perfShotsEvery);
		}
		lastShot = t;
	}
	emuCpu = emuNow;
	renderCpu = renderNow;
	windowStart = now;
	vblanks = 0;
	frames = FrameCount;
	skipped = fskip;
}

static u32 lightgun_line = 0xffff;
static u32 lightgun_hpos;
static bool maple_int_pending;

#ifdef LIBRETRO
extern void retro_setfps(double fps);
#endif

void CalculateSync()
{
	u32 pixel_clock = PIXEL_CLOCK / (FB_R_CTRL.vclk_div ? 1 : 2);

	// We need to calculate the pixel clock

	pvr_numscanlines = SPG_LOAD.vcount + 1;

	Line_Cycles = (u32)((u64)SH4_MAIN_CLOCK * (u64)(SPG_LOAD.hcount + 1) / (u64)pixel_clock);
	if (SPG_CONTROL.interlace)
		Line_Cycles /= 2;

	Frame_Cycles = pvr_numscanlines * Line_Cycles;
	prv_cur_scanline = 0;
	clc_pvr_scanline = 0;

	sh4_sched_request(vblank_schid, Line_Cycles);

#ifdef LIBRETRO
	retro_setfps((double)SH4_MAIN_CLOCK / ((double)Line_Cycles * (double)pvr_numscanlines));
#endif
}

static int getNextSpgInterrupt()
{
	if (SPG_HBLANK_INT.hblank_int_mode == 2)
		return Line_Cycles;

	u32 min_scanline = prv_cur_scanline + 1;
	u32 min_active = pvr_numscanlines;

	if (min_scanline <= SPG_VBLANK_INT.vblank_in_interrupt_line_number)
		min_active = std::min(min_active, SPG_VBLANK_INT.vblank_in_interrupt_line_number);

	if (min_scanline <= SPG_VBLANK_INT.vblank_out_interrupt_line_number)
		min_active = std::min(min_active, SPG_VBLANK_INT.vblank_out_interrupt_line_number);

	if (min_scanline <= SPG_VBLANK.vstart)
		min_active = std::min(min_active, SPG_VBLANK.vstart);

	if (min_scanline <= SPG_VBLANK.vbend)
		min_active = std::min(min_active, SPG_VBLANK.vbend);

	if (lightgun_line != 0xffff && min_scanline <= lightgun_line)
		min_active = std::min(min_active, lightgun_line);

	if (SPG_HBLANK_INT.hblank_int_mode == 0 && min_scanline <= SPG_HBLANK_INT.line_comp_val)
		min_active = std::min(min_active, SPG_HBLANK_INT.line_comp_val);

	min_active = std::max(min_active, min_scanline);

	return (min_active - prv_cur_scanline) * Line_Cycles;
}

void rescheduleSPG()
{
	sh4_sched_request(vblank_schid, getNextSpgInterrupt());
}

static int spg_line_sched(int tag, int cycles, int jitter, void *arg)
{
	clc_pvr_scanline += cycles + jitter;

	while (clc_pvr_scanline >= Line_Cycles)
	{
		prv_cur_scanline = (prv_cur_scanline + 1) % pvr_numscanlines;
		clc_pvr_scanline -= Line_Cycles;
		
		if (SPG_VBLANK_INT.vblank_in_interrupt_line_number == prv_cur_scanline)
		{
			if (maple_int_pending)
			{
				maple_int_pending = false;
				SB_MDST = 0;
			}
			asic_RaiseInterrupt(holly_SCANINT1);
		}

		if (SPG_VBLANK_INT.vblank_out_interrupt_line_number == prv_cur_scanline)
		{
			maple_vblank();
			asic_RaiseInterrupt(holly_SCANINT2);
		}

		if (SPG_VBLANK.vstart == prv_cur_scanline)
			SPG_STATUS.vsync = 1;

		if (SPG_VBLANK.vbend == prv_cur_scanline)
			SPG_STATUS.vsync = 0;

		SPG_STATUS.scanline = prv_cur_scanline;
		
		switch (SPG_HBLANK_INT.hblank_int_mode)
		{
		case 0:
			if (prv_cur_scanline == SPG_HBLANK_INT.line_comp_val)
				asic_RaiseInterrupt(holly_HBLank);
			break;
		case 2:
			asic_RaiseInterrupt(holly_HBLank);
			break;
		case 1:
			WARN_LOG(PVR, "Unimplemented HBLANK INT mode");
			break;
		default:
			INFO_LOG(PVR, "Invalid HBLANK INT mode");
			break;
		}

		// Vblank
		if (prv_cur_scanline == 0)
		{
			if (SPG_CONTROL.interlace)
				SPG_STATUS.fieldnum = ~SPG_STATUS.fieldnum;
			else
				SPG_STATUS.fieldnum = 0;

			rend_vblank();
			if (perfEnabled)
				perfVblank();

			u64 now = getTimeMs();
			cpu_time_idx = (cpu_time_idx + 1) % cpu_cycles.size();
			if (cpu_cycles[cpu_time_idx] != 0)
			{
				u32 cycle_span = (u32)(sh4_sched_now64() - cpu_cycles[cpu_time_idx]);
				u64 time_span = now - real_times[cpu_time_idx];
				float cpu_speed = ((float)cycle_span / time_span) / (SH4_MAIN_CLOCK / 100000);
				SH4FastEnough = cpu_speed >= 85.f;
			}
			else {
				SH4FastEnough = false;
			}
			cpu_cycles[cpu_time_idx] = sh4_sched_now64();
			real_times[cpu_time_idx] = now;

			// Deadcast: the lag is real time since the last vblank minus emulated time, summed while the SH4 runs
			// flat out, reset when the audio held it back, and capped at 50 ms (a load or pause is not lag to make up)
			{
				static std::chrono::steady_clock::time_point lastReal;
				static u64 lastCycles;
				const auto real = std::chrono::steady_clock::now();
				const u64 cycles = sh4_sched_now64();
				if (lastCycles != 0)
				{
					const float realMs = std::chrono::duration<float, std::milli>(real - lastReal).count();
					const float sh4Ms = (float)(cycles - lastCycles) / (SH4_MAIN_CLOCK / 1000);
					emulationLagMs = sh4HeldBack != 0 ? 0.f : std::clamp(emulationLagMs + realMs - sh4WaitedMs - sh4Ms, 0.f, 50.f);
				}
				sh4HeldBack = 0;
				sh4WaitedMs = 0.f;
				lastReal = real;
				lastCycles = cycles;
			}

#ifdef TEST_AUTOMATION
			replay_input();
#endif

#if !defined(NDEBUG) || defined(DEBUGFAST)
			vblk_cnt++;
			if (getTimeMs() - last_fps >= 2000)
			{
				static int Last_FC;
				double ts = ((double)getTimeMs() - last_fps) / 1000.0;
				double spd_fps = (FrameCount - Last_FC) / ts;
				double spd_vbs = vblk_cnt / ts;
				double spd_cpu = spd_vbs * Frame_Cycles;
				spd_cpu /= 1000000.0;	//mrhz kthx
				double fullvbs = (spd_vbs / spd_cpu) * 200.0;

				Last_FC=FrameCount;

				vblk_cnt=0;

				const char* mode=0;
				const char* res=0;

				res = SPG_CONTROL.interlace ? "480i" : "240p";

				if (SPG_CONTROL.isPAL())
					mode = "PAL";
				else if (SPG_CONTROL.isNTSC())
					mode = "NTSC";
				else
				{
					res = SPG_CONTROL.interlace ? "480i" : "480p";
					mode = "VGA";
				}

				double frames_done=spd_cpu/2;
				double mspdf=1/frames_done*1000;

				double full_rps = spd_fps + fskip / ts;

				INFO_LOG(COMMON, "SPG - %4.2f - %4.2f - V: %4.2f (%.2f, %s%s%4.2f) R: %4.2f+%4.2f",
					mspdf, spd_cpu * 100 / 200, spd_vbs,
					spd_vbs / full_rps, mode, res, fullvbs,
					spd_fps, fskip / ts);
				
				fskip = 0;
				last_fps = getTimeMs();
			}
#endif
		}
		if (lightgun_line != 0xffff && lightgun_line == prv_cur_scanline)
		{
			maple_int_pending = false;
			SPG_TRIGGER_POS = ((lightgun_line & 0x3FF) << 16) | (lightgun_hpos & 0x3FF);
			SB_MDST = 0;
			lightgun_line = 0xffff;
		}
	}

	return getNextSpgInterrupt();
}

void read_lightgun_position(int x, int y)
{
	static u32 flip;
	maple_int_pending = true;
	if (y < 0 || y >= 480 || x < 0 || x >= 640)
	{
		// Off screen
		lightgun_line = 0xffff;
	}
	else
	{
		lightgun_line = y / (SPG_CONTROL.interlace ? 2 : 1) + SPG_VBLANK_INT.vblank_out_interrupt_line_number;
		// For some reason returning the same position twice makes it register off screen.
		// Atomiswave Clay Challenge wants more wiggle than others for shots to register.
		lightgun_line ^= (flip >> 1) & 1;
		lightgun_hpos = (x + 286) ^ (flip & 1);
		flip++;
	}
}

bool spg_Init()
{
	render_end_schid = sh4_sched_register(0, &rend_end_render);
	vblank_schid = sh4_sched_register(0, &spg_line_sched);

	return true;
}

void spg_Term()
{
	sh4_sched_unregister(render_end_schid);
	render_end_schid = -1;
	sh4_sched_unregister(vblank_schid);
	vblank_schid = -1;
}

void spg_Reset(bool hard)
{
	CalculateSync();

	SH4FastEnough = false;
	cpu_time_idx = 0;
	cpu_cycles.fill(0);
	real_times.fill(0.0);
	maple_int_pending = false;
	lightgun_line = 0xffff;
	lightgun_hpos = 0;
}

void scheduleRenderDone(TA_context *cntx)
{
	int cycles = 4096;
	if (cntx != nullptr)
	{
		if (settings.platform.isNaomi2()) {
			cycles = 1500000;
		}
		else
		{
			int size = 0;
			for (TA_context *c = cntx; c != nullptr; c = c->nextContext)
				size += c->tad.thd_data - c->tad.thd_root;
			cycles = std::min(450000 + size * 100, 1500000);
		}
	}
	sh4_sched_request(render_end_schid, cycles);
}

void spg_Serialize(Serializer& ser)
{
	ser << clc_pvr_scanline;
	ser << maple_int_pending;
	ser << pvr_numscanlines;
	ser << prv_cur_scanline;
	ser << Line_Cycles;
	ser << Frame_Cycles;
	ser << lightgun_line;
	ser << lightgun_hpos;
}
void spg_Deserialize(Deserializer& deser)
{
	if (deser.version() < Deserializer::V30)
		deser.skip<u32>(); // in_vblank
	deser >> clc_pvr_scanline;
	deser >> maple_int_pending;
	deser >> pvr_numscanlines;
	deser >> prv_cur_scanline;
	deser >> Line_Cycles;
	deser >> Frame_Cycles;
	deser >> lightgun_line;
	deser >> lightgun_hpos;
}
