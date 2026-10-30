#include "hw/pvr/spg.h"
#include "aica.h"
#include "aica_if.h"
#include "aica_mem.h"
#include "sgc_if.h"
#include "hw/holly/holly_intc.h"
#include "hw/holly/sb.h"
#include "hw/sh4/sh4_sched.h"
#include "hw/arm7/arm7.h"
#include "hw/arm7/arm_mem.h"
#include "cfg/option.h"
#include "oslib/oslib.h"
#include "deadcast/profiler.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <algorithm>
#include <thread>

namespace aica
{

#define SH4_IRQ_BIT (1 << (holly_SPU_IRQ & 31))

std::deque<u8> midiSendBuffer;

//Interrupts
//arm side
static u32 GetL(u32 which)
{
	if (which > 7)
		which = 7; //higher bits share bit 7

	u32 bit = 1 << which;
	u32 rv = 0;

	if (CommonData->SCILV0 & bit)
		rv = 1;

	if (CommonData->SCILV1 & bit)
		rv |= 2;
	
	if (CommonData->SCILV2 & bit)
		rv |= 4;

	return rv;
}

static void update_arm_interrupts()
{
	u32 p_ints=SCIEB->full & SCIPD->full;

	u32 Lval=0;
	if (p_ints)
	{
		u32 bit_value=1;//first bit
		//scan all interrupts , lo to hi bit.I assume low bit ints have higher priority over others
		for (u32 i=0;i<11;i++)
		{
			if (p_ints & bit_value)
			{
				//for the first one , Set the L reg & exit
				Lval=GetL(i);
				break;
			}
			bit_value<<=1; //next bit
		}
	}

	arm::interruptChange(p_ints,Lval);
}

// Deadcast: with ThreadedAudio, the AICA (ARM7, channels, timers, DSP) runs on its own thread a few
// samples behind the SH4. The SH4's scheduler only counts the samples due (AicaUpdate); the AICA thread
// renders them in batches, paced by the audio output, and the SH4 waits when it gets more than MaxLag
// samples ahead. SH4 accesses to AICA registers, save, load and reset first sync() with the AICA
// thread. The AICA's interrupt to the SH4 is raised by the SH4 thread at its next sample.
namespace worker
{
constexpr u32 Batch = 64;		// made once this many samples are due
constexpr u32 Chunk = 64;		// the most the AICA thread makes at a time, so a held SH4 goes on soon
constexpr u32 MaxLag = 1024;	// the SH4 waits beyond this many samples ahead (23 ms)

static std::thread thread;
static std::mutex mutex;
static std::condition_variable wake;	// for the AICA thread: samples due, a sync, or stop
static std::condition_variable idle;	// for the SH4 thread: all caught up
static std::atomic<u32> pending;		// samples due
static bool busy;
static bool syncing;
static bool throttled;	// the SH4 is waiting for fewer than MaxLag samples due
static bool stopping;
static std::atomic<int> sh4Irq { -1 };	// what the SH4 interrupt must become: -1 unchanged, 0 or 1
static thread_local bool onWorker;

static bool running() {
	return thread.joinable();
}
}

static void setSh4Irq(bool raise)
{
	if (raise)
	{
		if ((SB_ISTEXT & SH4_IRQ_BIT) == 0)
			// if no interrupt is already pending then raise one
			asic_RaiseInterrupt(holly_SPU_IRQ);
	}
	else if ((SB_ISTEXT & SH4_IRQ_BIT) != 0)
		asic_CancelInterrupt(holly_SPU_IRQ);
}

// the SH4 interrupt raised by the AICA, as the AICA thread last left it
static void applySh4Irq()
{
	int irq = worker::sh4Irq.exchange(-1);
	if (irq != -1)
		setSh4Irq(irq == 1);
}

//sh4 side
static bool UpdateSh4Ints()
{
	u32 p_ints = MCIEB->full & MCIPD->full;
	if (worker::onWorker)
		worker::sh4Irq = p_ints != 0 ? 1 : 0;
	else
		setSh4Irq(p_ints != 0);
	return p_ints != 0;
}

AicaTimer timers[3];
int aica_schid = -1;
constexpr int AICA_TICK = 4535;		// 44.1 KHz
// Deadcast: with the AICA thread, samples due are counted this many at a time (1/16 of the events)
constexpr u32 TickSamples = 16;

static int AicaUpdate(int tag, int cycles, int jitter, void *arg)
{
	if (!worker::running())
	{
		arm::run(1);
		return AICA_TICK;
	}
	applySh4Irq();
	u32 due = worker::pending.fetch_add(TickSamples) + TickSamples;
	if (due > worker::MaxLag)
	{
		// as much as the AICA thread is behind, no more: not all of it (sync()), which would hold the
		// SH4 for as long as the audio output takes to play it all
		std::unique_lock<std::mutex> lock(worker::mutex);
		sh4HeldBack++;
		worker::throttled = true;
		worker::wake.notify_one();
		worker::idle.wait(lock, [] { return worker::pending <= worker::MaxLag - worker::Chunk; });
		worker::throttled = false;
	}

	return AICA_TICK * TickSamples;
}

static void workerLoop()
{
	ThreadName _("deadcast-aica");
	profilerStartThread("deadcast-aica");
	worker::onWorker = true;
	std::unique_lock<std::mutex> lock(worker::mutex);
	while (true)
	{
		// Deadcast: polls for samples due every 2 ms; the SH4 thread only wakes it when waiting itself
		worker::wake.wait_for(lock, std::chrono::milliseconds(2), [] {
			return worker::pending >= worker::Batch || (worker::syncing && worker::pending != 0) || worker::stopping;
		});
		if (worker::stopping && worker::pending == 0)
			break;
		worker::busy = true;
		lock.unlock();
		u32 samples = std::min<u32>(worker::pending, worker::Chunk);
		worker::pending -= samples;
		if (samples != 0)
			arm::run(samples);
		lock.lock();
		worker::busy = false;
		if (worker::pending < worker::Batch || worker::throttled)
			worker::idle.notify_all();
	}
}

void sync()
{
	if (!worker::running() || worker::onWorker)
		return;
	std::unique_lock<std::mutex> lock(worker::mutex);
	if (worker::pending != 0 || worker::busy)
	{
		worker::syncing = true;
		worker::wake.notify_one();
		worker::idle.wait(lock, [] { return worker::pending == 0 && !worker::busy; });
		worker::syncing = false;
	}
	lock.unlock();
	applySh4Irq();
}

static void startWorker()
{
	if (worker::running() || !config::ThreadedAudio)
		return;
	worker::stopping = false;
	worker::pending = 0;
	worker::thread = std::thread(workerLoop);
}

static void stopWorker()
{
	if (!worker::running())
		return;
	{
		std::lock_guard<std::mutex> lock(worker::mutex);
		worker::stopping = true;
	}
	worker::wake.notify_one();
	worker::thread.join();
	applySh4Irq();
}

//Mainloop

void timeStep()
{
	for (auto& timer : timers)
		timer.StepTimer(1);

	SCIPD->SAMPLE_DONE = 1;
	MCIPD->SAMPLE_DONE = 1;

	sgc::AICA_Sample();

	//Make sure sh4/arm interrupt system is up to date
	update_arm_interrupts();
	UpdateSh4Ints();	
}

static void AicaInternalDMA()
{
	if (!CommonData->DEXE)
		return;

	// Start dma
	DEBUG_LOG(AICA, "AICA internal DMA: DGATE %d DDIR %d DLG %x", CommonData->DGATE, CommonData->DDIR, CommonData->DLG);
	if (CommonData->DGATE)
	{
		// Clear memory/registers
		if (CommonData->DDIR)
		{
			// to wave mem
			u32 addr = ((CommonData->DMEA_hi << 16) | (CommonData->DMEA_lo << 2)) & ARAM_MASK;
			u32 len = std::min(CommonData->DLG, ARAM_SIZE - addr);
			memset(&aica_ram[addr], 0, len * 4);
		}
		else
		{
			// to regs
			u32 addr = CommonData->DRGA << 2;
			for (u32 i = 0; i < CommonData->DLG; i++, addr += 4)
				writeAicaReg(addr, (u32)0);
		}
	}
	else
	{
		// Data xfer
		u32 waddr = ((CommonData->DMEA_hi << 16) | (CommonData->DMEA_lo << 2)) & ARAM_MASK;
		u32 raddr = CommonData->DRGA << 2;
		u32 len = std::min(CommonData->DLG, ARAM_SIZE - waddr);
		if (CommonData->DDIR)
		{
			// reg to wave mem
			for (u32 i = 0; i < len; i++, waddr += 4, raddr += 4)
				*(u32*)&aica_ram[waddr] = readAicaReg<u32>(raddr);
		}
		else
		{
			// wave mem to regs
			for (u32 i = 0; i < len; i++, waddr += 4, raddr += 4)
				writeAicaReg(raddr, *(u32*)&aica_ram[waddr]);
		}
	}
	CommonData->DEXE = 0;
	MCIPD->DMA_END = 1;
	UpdateSh4Ints();
	SCIPD->DMA_END = 1;
	update_arm_interrupts();
}

//Memory i/o
template<typename T>
void writeTimerAndIntReg(u32 reg, T data)
{
	switch (reg)
	{
	case SCIEB_addr:
		SCIEB->full = data & 0x7ff;
		update_arm_interrupts();
		break;

	case SCIPD_addr:
		// other bits are read-only
		if (data & (1 << 5))
		{
			SCIPD->SCPU = 1;
			update_arm_interrupts();
		}
		break;

	case SCIRE_addr:
		SCIPD->full &= ~data;
		update_arm_interrupts();
		break;

	case MCIEB_addr:
		MCIEB->full = data & 0x7ff;
		if (UpdateSh4Ints())
			arm::avoidRaceCondition();
		break;

	case MCIPD_addr:
		// other bits are read-only
		if (data & (1 << 5))
		{
			MCIPD->SCPU = 1;
			if (UpdateSh4Ints())
				arm::avoidRaceCondition();
		}
		break;

	case MCIRE_addr:
		MCIPD->full &= ~data;
		UpdateSh4Ints();
		break;

	case TIMER_A:
		WriteMemArr(aica_reg, reg, data);
		timers[0].RegisterWrite();
		break;

	case TIMER_B:
		WriteMemArr(aica_reg, reg, data);
		timers[1].RegisterWrite();
		break;

	case TIMER_C:
		WriteMemArr(aica_reg, reg, data);
		timers[2].RegisterWrite();
		break;

	// DEXE, DDIR, DLG
	case 0x288C:
		WriteMemArr(aica_reg, reg, data);
		AicaInternalDMA();
		break;

	default:
		WriteMemArr(aica_reg, reg, data);
		break;
	}
}

template void writeTimerAndIntReg<>(u32 reg, u8 data);
template void writeTimerAndIntReg<>(u32 reg, u16 data);
template void writeTimerAndIntReg<>(u32 reg, u32 data);

void midiSend(u8 data)
{
	sync();
	midiSendBuffer.push_back(data);
	SCIPD->MIDI_IN = 1;
	update_arm_interrupts();
	MCIPD->MIDI_IN = 1;
	UpdateSh4Ints();
}

void init()
{
	initMem();
	initRtc();

	sgc::init();
	if (aica_schid == -1)
		aica_schid = sh4_sched_register(0, &AicaUpdate);
	arm::init();
	startWorker();
}

void reset(bool hard)
{
	sync();
	if (hard)
	{
		initMem();
		sgc::term();
		sgc::init();
		sh4_sched_request(aica_schid, AICA_TICK);
	}
	for (std::size_t i = 0; i < std::size(timers); i++)
		timers[i].Init(aica_reg, i);
	resetRtc(hard);
	arm::reset();
}

void term()
{
	stopWorker();
	arm::term();
	sgc::term();
	termMem();
	sh4_sched_unregister(aica_schid);
	aica_schid = -1;
}

} // namespace aica
