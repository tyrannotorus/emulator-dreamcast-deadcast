#include "waitloops.h"
#include "hw/sh4/sh4_if.h"
#include "hw/sh4/sh4_sched.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/dyna/blockmanager.h"
#include "hw/sh4/modules/mmu.h"

#include <algorithm>
#include <array>

namespace deadcast
{

u32 waitLoopTurnsSkipped;
u32 sleepTimeslices;

static bool codeIs(u32 addr, const u16 *code, size_t count)
{
	for (size_t i = 0; i < count; i++)
		if (ReadMem16_nommu(addr + i * 2) != code[i])
			return false;
	return true;
}

// Lets the SH4's time go by as turns of a loop taking `turn` cycles each would, timeslice by timeslice
// as the main loop does it (its intc_sched), until an interrupt is pending, holds() no longer does, or
// maxTurns. The loop runs on by itself from there: an interrupt is taken at the next timeslice, as it
// would have been, a timeslice late at most. And no more than 10 ms of the SH4's time a call, so the
// emulator can stop meanwhile.
// holds() is asked again only after the scheduler ran its events: while the SH4 waits, nothing else
// writes main RAM (DMA and the rest land in those events, and interrupts stop the loop), so it could not
// have changed in between.
template<typename Holds>
static u32 passTurns(u32 turn, u32 maxTurns, Holds holds)
{
	Sh4Context& ctx = Sh4cntx;
	u32 done = 0;
	u32 cycles = 0;
	bool ask = true;
	while (done < maxTurns && cycles < SH4_MAIN_CLOCK / 100 && !ctx.interrupt_pend)
	{
		if (ask)
		{
			if (!holds())
				break;
			ask = false;
		}
		if (ctx.cycle_counter <= 0)
		{
			ctx.cycle_counter += SH4_TIMESLICE;
			ctx.sh4_sched_next -= SH4_TIMESLICE;
			if (ctx.sh4_sched_next < 0)
			{
				sh4_sched_tick(SH4_TIMESLICE);
				ask = true;
			}
			continue;
		}
		u32 turns = std::min<u32>(maxTurns - done, ctx.cycle_counter / turn + 1);
		done += turns;
		cycles += turns * turn;
		ctx.cycle_counter -= turns * turn;
	}
	waitLoopTurnsSkipped += done;
	return done;
}

// The SH4's clock, in cycles: the scheduler's, and the main loop's countdown into the timeslice
static u64 sh4Now()
{
	return (u64)((s64)sh4_sched_now64() - Sh4cntx.cycle_counter);
}

// How long a turn of a loop takes, measured from one turn to the next where the loop's hook is
// called each turn: known once two turns in a row took the same time.
struct TurnClock
{
	u64 last = 0;
	u32 lastTurn = 0;
	u32 turn = 0;

	// at the hook: whether the turn just ended took the known time
	bool steady()
	{
		u64 now = sh4Now();
		u64 took = now - last;
		last = now;
		if (turn != 0 && took == turn)
			return true;
		if (took == lastTurn && took < 4096)
			turn = (u32)took;
		lastTurn = (u32)took;
		return false;
	}
	// after turns went by without running: the next turn is measured from here
	void restart() {
		last = sh4Now();
	}
};

// The Katana library's wait for the vblank counter to change (Dead or Alive 2, Tennis 2K2), a timeout
// counter running down, wherever a game has it:
//   jsr @R13 ; mov #7,R4     calls the callback dispatcher for slot 7, a no-op function (rts; nop)
//   mov.l @R15,R3 ; mov.l @R3,R2 ; add #-1,R2 ; mov.l R2,@R3      (*counter)--
//   mov.l @(4,R15),R3 ; mov.l @R3,R2 ; cmp/eq R14,R2 ; bf exit    *flag != R14: done
//   mov.l @R15,R1 ; mov.l @R1,R3 ; tst R3,R3 ; bf <the jsr>        until *counter == 0
// While the flag holds and the callback is the no-op, a turn of the loop only takes a count off the
// counter, and the time its blocks take.
static constexpr std::array<u16, 14> KatanaWaitCode {
	0x4d0b, 0xe407,							// jsr @R13 ; mov #7,R4
	0x63f2, 0x6232, 0x72ff, 0x2322,			// mov.l @R15,R3 ; mov.l @R3,R2 ; add #-1,R2 ; mov.l R2,@R3
	0x53f1, 0x6232, 0x32e0, 0x8b03,			// mov.l @(4,R15),R3 ; mov.l @R3,R2 ; cmp/eq R14,R2 ; bf
	0x61f2, 0x6312, 0x2338, 0x8bf1,			// mov.l @R15,R1 ; mov.l @R1,R3 ; tst R3,R3 ; bf
};
// the dispatcher: calls table[R4].f(table[R4].arg), the table at its literal (mov.l @(0x38,PC),R3)
static constexpr std::array<u16, 17> KatanaDispatcherCode {
	0x4f22, 0x7ff8, 0x6243, 0xd30e, 0x4208, 0x2f42, 0x4200, 0x323c, 0x6423, 0x1f21,
	0x6122, 0x410b, 0x5441, 0x7f08, 0x4f26, 0x000b, 0xe000,
};
static TurnClock katanaClock;

static bool inMainRam(u32 a) {
	return (a & 0x1C000000) == 0x0C000000;
}

static void DYNACALL katanaWaitSkip(u32 vaddr)
{
	Sh4Context& ctx = Sh4cntx;
	const u32 dispatcher = ctx.r[13];
	if (!katanaClock.steady() || !inMainRam(dispatcher) || (dispatcher & 1) != 0
			|| !codeIs(dispatcher, KatanaDispatcherCode.data(), KatanaDispatcherCode.size()))
		return;
	// slot 7 of the dispatcher's table, still the no-op?
	const u32 table = ReadMem32_nommu(((dispatcher + 6) & ~3) + 4 + 0x0e * 4);
	if (!inMainRam(table) || (table & 3) != 0)
		return;
	const u32 noOp = ReadMem32_nommu(table + 7 * 8);
	if (!inMainRam(noOp) || (noOp & 1) != 0 || ReadMem16_nommu(noOp) != 0x000b || ReadMem16_nommu(noOp + 2) != 0x0009)
		return;
	const u32 counterAddr = ReadMem32_nommu(ctx.r[15]);
	const u32 flagAddr = ReadMem32_nommu(ctx.r[15] + 4);
	if (!inMainRam(counterAddr) || !inMainRam(flagAddr) || ((counterAddr | flagAddr) & 3) != 0)
		return;
	u32 counter = ReadMem32_nommu(counterAddr);
	// short of the counter running out: the loop ends then by itself
	if (counter > 1 && ReadMem32_nommu(flagAddr) == ctx.r[14])
	{
		counter -= passTurns(katanaClock.turn, counter - 1, [&] { return ReadMem32_nommu(flagAddr) == ctx.r[14]; });
		WriteMem32_nommu(counterAddr, counter);
		katanaClock.restart();
	}
}

// Sega Rally 2 (WinCE), waiting for the next frame: its loop asks F for the frame count until it
// changes from R12's,
//   bsr F ; nop
//   cmp/eq R12,R0 ; bt <the bsr>          (the hook's block)
// F asks an object's method M for it: hr = obj->vtbl[24](obj, &out); return hr >= 0 ? out : 0, obj
// at F's literal. M: v = **g1 - *g2 (**g1 counted at vblank, by its interrupt); by *g3: 1 v, 2 v >> 1,
// 4 v >> 2 (3 calls a function); *out = v; *g4 = 0, the g at M's literals. All of it in RAM, nothing
// the loop reads changes but by an interrupt, and a turn writes the same each time.
static constexpr std::array<u16, 19> RallyF {
	0x4f22, 0x7fec, 0xd139, 0xe060, 0x6412, 0xe510, 0x6242, 0x022e, 0x420b, 0x35fc,
	0x4011, 0x8901, 0xa001, 0xe000, 0x50f4, 0x7f14, 0x4f26, 0x000b, 0x0009,
};
static constexpr u32 RallyFObject = 0xec;		// F's literal: where obj is
static constexpr std::array<u16, 53> RallyM {
	0x2f86, 0x4f22, 0x6853, 0x7ff0, 0x2888, 0x8927, 0xd435, 0x6442, 0x2448, 0x8923,
	0xd334, 0x6442, 0x6332, 0x3438, 0xd133, 0xe203, 0x6512, 0x75ff, 0x3526, 0x8915,
	0xe103, 0x3150, 0x8910, 0x2558, 0x8904, 0xe101, 0x3150, 0x8903, 0xa005, 0x0009,
	0xa00a, 0x2842, 0x4401, 0xa007, 0x2842, 0xd024, 0x400b, 0xe503, 0xa002, 0x2802,
	0x4409, 0x2842, 0xd21f, 0xe000, 0xa003, 0x2202, 0xd21d, 0xd01c, 0x2202, 0x7f10,
	0x4f26, 0x000b, 0x68f6,
};
static constexpr u32 RallyMG1 = 0xe4, RallyMG2 = 0xe8, RallyMG3 = 0xec, RallyMG4 = 0xd4;	// M's literals

static bool isRam(u32 pa) {
	return (pa & 0x1C000000) == 0x0C000000;
}

// the word at va, if the MMU maps it to RAM now
static bool ramWord(u32 va, u32& value)
{
	u32 pa;
	if (!mmu_peek(va, pa) || !isRam(pa))
		return false;
	value = ReadMem32_nommu(pa);
	return true;
}

// whether the code at va, in RAM and in one page, is this code
template<size_t N>
static bool ramCode(u32 va, const std::array<u16, N>& code)
{
	u32 pa;
	return (va & 3) == 0 && (va & 0xfff) + N * 2 <= 0x1000 && mmu_peek(va, pa) && isRam(pa)
			&& codeIs(pa, code.data(), N);
}

// whether the loop ending at tail is the one above, all of it, as the MMU maps it now
static bool rallyLoopHolds(u32 tail)
{
	u32 pa;
	if (!mmu_peek(tail - 4, pa) || !isRam(pa))
		return false;
	const u16 bsr = ReadMem16_nommu(pa);
	const u32 f = tail + ((s32)((bsr & 0xfff) << 20) >> 19);
	u32 obj, vtbl, m, g1, g2, g3, g4, counter, word, mode;
	const u32 sp = Sh4cntx.r[15];
	return (bsr & 0xf000) == 0xb000 && ramCode(f, RallyF)
			&& ramWord(f + RallyFObject, obj) && ramWord(obj, obj) && ramWord(obj, vtbl) && ramWord(vtbl + 24 * 4, m)
			&& ramCode(m, RallyM)
			&& ramWord(m + RallyMG1, g1) && ramWord(m + RallyMG2, g2) && ramWord(m + RallyMG3, g3) && ramWord(m + RallyMG4, g4)
			&& ramWord(g1, counter) && counter != 0 && ramWord(counter, word) && ramWord(g2, word)
			&& ramWord(g3, mode) && (mode == 1 || mode == 2 || mode == 4) && ramWord(g4, word)
			// the stack F and M use
			&& ramWord(sp - 48, word) && ramWord(sp - 4, word);
}

static TurnClock rallyClock;

static void DYNACALL rallySkip(u32 tail)
{
	Sh4Context& ctx = Sh4cntx;
	// R0: what F returned this turn; the same as R12, the loop goes on
	if (!rallyClock.steady() || ctx.r[0] != ctx.r[12] || ctx.interrupt_pend || !rallyLoopHolds(tail))
		return;
	passTurns(rallyClock.turn, ~0u, [] { return true; });
	rallyClock.restart();
}

// NBA 2K2 (USA), waiting for a frame counter to move on, with a 0.125 s timeout on the SH4's timer:
//   0c237612  [[a0] + 0x34] != 0 and [[a1] + 0x18] == [R12 + 0x28]: waits on
//   0c237624  else [R9 + 0x2c] is 1 or 2 and [[a2] + 0x10] == [R12 + 0x28]: waits on; else done
//   0c23763a  elapsed = time() - R14:R13, made a float bit by bit (most of a turn's time, 0c23dce0),
//             until elapsed > 0.125 s
// (a0, a1, a2: the loop's literals.) Nothing a turn reads changes but by an interrupt or the
// scheduler's events, and a turn writes nothing but below the stack. Its turns are not all alike (the
// conversion takes as long as elapsed has bits), so the last one's time stands for them.
static constexpr u32 Nba2k2Head = 0x0c237612;
static constexpr std::array<u16, 36> Nba2k2Code {
	0xd03b, 0x510d, 0x2118, 0x8904, 0xd03a, 0x5206, 0x51ca, 0x3210, 0x890a, 0x509b,
	0x8801, 0x8902, 0x509b, 0x8802, 0x8b27, 0xd035, 0x51ca, 0x5204, 0x3210, 0x8b22,
	0xd02c, 0xd42c, 0x400b, 0x0009, 0x6403, 0xd031, 0x0008, 0x6513, 0x34da, 0x35ea,
	0x400b, 0x0009, 0xc72f, 0xf108, 0xf015, 0x8bdb,
};
static constexpr u32 Nba2k2A0 = 0x0c237700, Nba2k2A1 = 0x0c237704, Nba2k2A2 = 0x0c237708;
static constexpr u32 Nba2k2Time = 0x0c2376ec, Nba2k2Convert = 0x0c23770c;	// the functions' literals
static constexpr std::array<u16, 40> Nba2k2TimeCode {
	0x5144, 0x2f86, 0x2f96, 0x2118, 0x8905, 0x6142, 0x5241, 0x6013, 0x6123, 0xa019,
	0x0009, 0x6842, 0x5941, 0xd20d, 0xd70d, 0x6322, 0x5176, 0x6617, 0x6122, 0x3310,
	0x8bf9, 0x6133, 0x6233, 0x4200, 0x322a, 0x6213, 0x6363, 0xe100, 0x6413, 0x213b,
	0x0008, 0x224b, 0x318a, 0x329a, 0x6013, 0x6123, 0x69f6, 0x68f6, 0x000b, 0x0009,
};
static constexpr std::array<u16, 21> Nba2k2ConvertCode {
	0x2558, 0x8b00, 0x2448, 0xd209, 0xf08d, 0x890c, 0xe101, 0x2149, 0x2118, 0x8901,
	0xf128, 0xf010, 0x4501, 0x4425, 0x2558, 0x8b00, 0x2448, 0x7204, 0x8bf2, 0x000b, 0x0009,
};

// the word at a, if it is in main RAM
// a word of main RAM, read straight from it (not through the address space's handlers: this is asked
// many times a second while a game waits)
static bool mainRamWord(u32 a, u32& value)
{
	if ((a & 3) != 0 || (a & 0x1C000000) != 0x0C000000)
		return false;
	value = *(const u32 *)&mem_b[a & RAM_MASK];
	return true;
}

static bool nba2k2Waits()
{
	const Sh4Context& ctx = Sh4cntx;
	u32 target, a, v;
	if (!mainRamWord(ctx.r[12] + 0x28, target))
		return false;
	if (mainRamWord(Nba2k2A0, a) && mainRamWord(a + 0x34, v) && v != 0
			&& mainRamWord(Nba2k2A1, a) && mainRamWord(a + 0x18, v) && v == target)
		return true;
	u32 state;
	return mainRamWord(ctx.r[9] + 0x2c, state) && (state == 1 || state == 2)
			&& mainRamWord(Nba2k2A2, a) && mainRamWord(a + 0x10, v) && v == target;
}

static u64 nba2k2Last;

static void DYNACALL nba2k2Skip(u32 vaddr)
{
	// a turn just went by: the time since the last call here
	const u64 now = sh4Now();
	const u64 took = now - nba2k2Last;
	nba2k2Last = now;
	if (took > 100000 || Sh4cntx.interrupt_pend || !nba2k2Waits())
		return;
	passTurns((u32)took, ~0u, nba2k2Waits);
	nba2k2Last = sh4Now();
}

WaitLoopHook waitLoopHook(const RuntimeBlockInfo *block)
{
	if (!mmu_enabled() && block->vaddr == Nba2k2Head && codeIs(Nba2k2Head, Nba2k2Code.data(), Nba2k2Code.size())
			&& codeIs(ReadMem32_nommu(Nba2k2Time), Nba2k2TimeCode.data(), Nba2k2TimeCode.size())
			&& codeIs(ReadMem32_nommu(Nba2k2Convert), Nba2k2ConvertCode.data(), Nba2k2ConvertCode.size()))
	{
		NOTICE_LOG(DYNAREC, "Deadcast: wait loop at %08x (NBA 2K2): skipped while waiting", block->vaddr);
		return nba2k2Skip;
	}
	if (!mmu_enabled() && inMainRam(block->vaddr) && (block->vaddr & 1) == 0
			&& codeIs(block->vaddr, KatanaWaitCode.data(), KatanaWaitCode.size()))
	{
		NOTICE_LOG(DYNAREC, "Deadcast: wait loop at %08x (the Katana library's vblank wait): skipped while waiting", block->vaddr);
		return katanaWaitSkip;
	}
	// cmp/eq R12,R0 ; bt -5, after bsr ; nop in the same page
	if ((block->vaddr & 0xfff) >= 4 && ReadMem16_nommu(block->addr) == 0x30c0 && ReadMem16_nommu(block->addr + 2) == 0x89fb
			&& ReadMem16_nommu(block->addr - 2) == 0x0009 && (ReadMem16_nommu(block->addr - 4) & 0xf000) == 0xb000)
	{
		NOTICE_LOG(DYNAREC, "Deadcast: wait loop at %08x (Sega Rally 2?): skipped while waiting, if it is", block->vaddr);
		return rallySkip;
	}
	return nullptr;
}

}
