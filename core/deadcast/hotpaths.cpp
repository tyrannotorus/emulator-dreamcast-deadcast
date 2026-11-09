#include "hotpaths.h"
#include "hw/sh4/sh4_if.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/dyna/blockmanager.h"
#include "hw/sh4/dyna/decoder.h"
#include "hw/sh4/modules/mmu.h"

#include <array>
#include <cstring>

namespace deadcast
{
namespace
{

template<size_t N>
bool codeIs(u32 addr, const std::array<u16, N>& code)
{
	for (size_t i = 0; i < N; i++)
		if (ReadMem16_nommu(addr + (u32)i * 2) != code[i])
			return false;
	return true;
}

bool inMainRam(u32 addr) {
	return (addr & 0x1C000000) == 0x0C000000;
}

// a float of main RAM, aligned
bool ramFloat(u32 addr, float& value)
{
	if (!inMainRam(addr) || (addr & 3) != 0)
		return false;
	memcpy(&value, &mem_b[addr & RAM_MASK], sizeof(value));
	return true;
}

// The 2K games' engine (NFL 2K2 at 0c222c8c, NBA 2K2 at 0c23d940): the sum, into FR0, of the floats from R2 on that
// the 64-bit mask R5:R4 picks, lowest bit first. Its loop, 5 to 6 SH4 instructions a bit in 3 blocks (9% of NFL 2K2's
// SH4 thread):
//   L  mov #1,R1 ; and R4,R1 ; tst R1,R1 ; bt S       (the block this is called from)
//   F  fmov.s @R2,FR1 ; fadd FR1,FR0                   (falls into S)
//   S  shlr R5 ; rotcr R4 ; tst R5,R5 ; bf N
//   Z  tst R4,R4                                       (falls into N)
//   N  add #4,R2 ; bf L
//   E  rts ; nop
// Blocks, as the decoder splits it: L (4), F+S (6), S (4), Z+N (3), N (2).
constexpr std::array<u16, 13> MaskSumCode {
	0xe101, 0x2149, 0x2118, 0x8901, 0xf128, 0xf010, 0x4501, 0x4425, 0x2558, 0x8b00, 0x2448, 0x7204, 0x8bf2
};
constexpr u32 MaskSumF = 8, MaskSumS = 12, MaskSumZ = 20, MaskSumN = 22, MaskSumE = 26;
// the blocks' cycles (dec_blockCycles), set when a block of the loop is compiled
s32 maskSumL, maskSumF, maskSumS, maskSumZ, maskSumN;

u32 DYNACALL maskSum(u32 head)
{
	Sh4Context& ctx = Sh4cntx;
	u32 r1 = ctx.r[1], r2 = ctx.r[2], r4 = ctx.r[4], r5 = ctx.r[5];
	float fr0 = ctx.fr[0], fr1 = ctx.fr[1];
	s32 cycles = ctx.cycle_counter;
	// FR1 and FR0 single floats: else the dynarec's blocks, compiled for the mode there is
	const bool single = ctx.fpscr.PR == 0 && ctx.fpscr.SZ == 0;
	u32 t;
	u32 pc;
	for (;;)
	{
		// L, its cycles charged by its block
		r1 = r4 & 1;
		if (r1 != 0)
		{
			t = 0;
			pc = head + MaskSumF;
			float value;
			if (cycles < 0 || !single || !ramFloat(r2, value))
				break;
			cycles -= maskSumF;
			fr1 = value;
			fr0 = fr0 + fr1;
		}
		else
		{
			t = 1;
			pc = head + MaskSumS;
			if (cycles < 0)
				break;
			cycles -= maskSumS;
		}
		// S: shlr R5 ; rotcr R4 ; tst R5,R5
		r4 = (r4 >> 1) | (r5 << 31);
		r5 >>= 1;
		if (r5 != 0)
		{
			// bf N taken
			t = 0;
			pc = head + MaskSumN;
			if (cycles < 0)
				break;
			cycles -= maskSumN;
		}
		else
		{
			t = 1;
			pc = head + MaskSumZ;
			if (cycles < 0)
				break;
			cycles -= maskSumZ;
			t = r4 == 0;
		}
		r2 += 4;
		if (t != 0)
		{
			// bf L not taken: the end
			pc = head + MaskSumE;
			break;
		}
		pc = head;
		if (cycles < 0)
			break;
		cycles -= maskSumL;
	}
	ctx.r[1] = r1;
	ctx.r[2] = r2;
	ctx.r[4] = r4;
	ctx.r[5] = r5;
	ctx.sr.T = t;
	ctx.fr[0] = fr0;
	ctx.fr[1] = fr1;
	ctx.cycle_counter = cycles;
	return pc;
}

} // namespace

HotPath hotPath(const RuntimeBlockInfo *block)
{
	const u32 head = block->vaddr;
	if (mmu_enabled() || !inMainRam(head) || (head & 1) != 0)
		return nullptr;
	if (codeIs(head, MaskSumCode))
	{
		maskSumL = dec_blockCycles(head, 4);
		maskSumF = dec_blockCycles(head + MaskSumF, 6);
		maskSumS = dec_blockCycles(head + MaskSumS, 4);
		maskSumZ = dec_blockCycles(head + MaskSumZ, 3);
		maskSumN = dec_blockCycles(head + MaskSumN, 2);
		NOTICE_LOG(DYNAREC, "Deadcast: the 2K engine's masked sum at %08x, native (cycles %d %d %d %d %d)", head, maskSumL,
				maskSumF, maskSumS, maskSumZ, maskSumN);
		return maskSum;
	}
	return nullptr;
}

}
