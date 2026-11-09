/*
	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
#include "sh4_if.h"
#include "sh4_mem.h"
#include "modules/mmu.h"
#include "hw/pvr/pvr_mem.h"
#include "hw/pvr/ta.h"
#include "modules/ccn.h"

static u32 CCN_QACR_TR[2];

template<bool mmu_on>
static void DYNACALL sqWrite(u32 dest, Sh4Context *ctx)
{
	u32 address;
	//Translate the SQ addresses as needed
	if (mmu_on)
	{
		mmu_TranslateSQW(dest, &address);
	}
	else
	{
		//sanity/optimisation check
		//verify(CCN_QACR_TR[0]==CCN_QACR_TR[1]);

		u32 QACR = CCN_QACR_TR[0];
		//QACR has already 0xE000_0000
		address = QACR + (dest & ~0x1f);
	}

	if (((address >> 26) & 7) != 4)//Area 4
	{
		const SQBuffer *sq = &ctx->sq_buffer[(dest >> 5) & 1];
		WriteMemBlock_nommu_sq(address, sq);
	}
	else
	{
		TAWriteSQ(address, ctx->sq_buffer);	// TODO pass the correct SQBuffer instead of letting TAWriteSQ deal with it
	}
}

u32 DYNACALL sqWriteMmuTry(u32 dest, Sh4Context *ctx)
{
	u32 address;
	const SqPage& sq = sqPages[CCN_PTEH.ASID];
	if ((dest & ~0xfff) == sq.vpage)
		address = sq.ppage | (dest & 0xfe0);
	else if (mmu_full_SQ<MMU_TT_DREAD>(dest, address) != MmuError::NONE)
		return 1;
	if (((address >> 26) & 7) == 4 && (address & 0x01FFFFE0) < 0x800000)
		// the TA's polygon FIFO (TAWriteSQ)
		ta_vtx_data32(&ctx->sq_buffer[(address >> 5) & 1]);
	else if (((address >> 26) & 7) != 4)
		WriteMemBlock_nommu_sq(address, &ctx->sq_buffer[(dest >> 5) & 1]);
	else
		TAWriteSQ(address, ctx->sq_buffer);
	return 0;
}

// Deadcast: the MMU on for the store queues alone (not emulated in full: mmu_set_state), as sports games
// have it: the queue's 1 MB page remapped (sq_remap) right here, and the TA's polygon FIFO fed straight
static void DYNACALL sqWriteRemap(u32 dest, Sh4Context *ctx)
{
	const u32 address = sq_remap[(dest >> 20) & 0x3F] | (dest & 0xFFFE0);
	const SQBuffer *sq = &ctx->sq_buffer[(dest >> 5) & 1];
	if (((address >> 26) & 7) != 4)
		WriteMemBlock_nommu_sq(address, sq);
	else if ((address & 0x01FFFFE0) < 0x800000)
		ta_vtx_data32(sq);
	else
		TAWriteSQ(address, ctx->sq_buffer);
}

//yes, this micro optimization makes a difference
static void DYNACALL sqWrite_nommu_area_3(u32 dest, Sh4Context *ctx)
{
	SQBuffer *pmem = (SQBuffer *)((u8 *)ctx + sizeof(Sh4Context) + 0x0C000000);
	pmem += (dest & (RAM_SIZE_MAX - 1)) >> 5;
	*pmem = ctx->sq_buffer[(dest >> 5) & 1];
}

static void DYNACALL sqWrite_nommu_area_3_nonvmem(u32 dest, Sh4Context *ctx)
{
	u8* pmem = &mem_b[0];

	memcpy((SQBuffer *)&pmem[dest & (RAM_MASK - 0x1F)], &ctx->sq_buffer[(dest >> 5) & 1], sizeof(SQBuffer));
}

static void DYNACALL sqWriteTA(u32 dest, Sh4Context *ctx)
{
	TAWriteSQ(dest, ctx->sq_buffer);
}

// Deadcast: a store queue area's 1 MB, remapped (sqWriteRemap) to main RAM: where in it, for the dynarec's flush to
// write the 32 bytes straight there (rec_arm64.cpp, sqFlush) as WriteMemBlock_nommu_sq does; else nullptr
u8 *sqRamRegions[64];

void sqTaRegionsUpdate()
{
	Sh4Context& ctx = p_sh4rcb->cntx;
	u8 *regions = (u8 *)&ctx + SqTaRegionsOffset;
	for (u32 k = 0; k < 64; k++)
	{
		bool ta = false;
		sqRamRegions[k] = nullptr;
		if (ctx.doSqWrite == &sqWriteRemap)
		{
			// the address: sq_remap[k] | (dest & 0xFFFE0), area 4 and below the YUV converter's
			const u32 a = sq_remap[k];
			ta = ((a >> 26) & 7) == 4 && ((a & 0x01FFFFE0) | 0xFFFE0) < 0x800000;
			// in main RAM, the 1 MB whole (GetMemPtr's test, for its last 32 bytes too)
			if (((a >> 29) & 7) != 7 && ((a >> 26) & 7) == 3 && (a & 0xFFFE0) == 0 && (a & RAM_MASK) + 0x100000 <= RAM_SIZE)
				sqRamRegions[k] = &mem_b[a & RAM_MASK];
		}
		else if (ctx.doSqWrite == &sqWriteTA)
			// TAWriteSQ(dest): dest's bits 23-24 clear
			ta = (k & 0x18) == 0;
		regions[k] = ta;
	}
}

void setSqwHandler()
{
	Sh4Context& ctx = p_sh4rcb->cntx;
	if (CCN_MMUCR.AT == 1)
	{
		ctx.doSqWrite = mmu_enabled() ? &sqWrite<true> : &sqWriteRemap;
	}
	else
	{
		u32 area = CCN_QACR0.Area;

		CCN_QACR_TR[0] = (area << 26) - 0xE0000000; //-0xE0000000 because 0xE0000000 is added on the translation again ...

		switch (area)
		{
		case 3:
			if (addrspace::virtmemEnabled())
				ctx.doSqWrite = &sqWrite_nommu_area_3;
			else
				ctx.doSqWrite = &sqWrite_nommu_area_3_nonvmem;
			break;

		case 4:
			ctx.doSqWrite = &sqWriteTA;
			break;

		default:
			ctx.doSqWrite = &sqWrite<false>;
			break;
		}
	}
	sqTaRegionsUpdate();
}
