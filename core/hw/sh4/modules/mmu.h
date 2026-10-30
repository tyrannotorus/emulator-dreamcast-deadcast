#pragma once
#include "types.h"
#include "hw/sh4/sh4_mmr.h"
#include "hw/sh4/dyna/ngen.h"
#include "hw/mem/addrspace.h"
#include "deadcast/mmuwindows.h"

//Translation Types
//Opcode read
#define MMU_TT_IREAD 0
//Data write
#define MMU_TT_DWRITE 1
//Data write
#define MMU_TT_DREAD 2

enum class MmuError
{
	//Translation was successful
	NONE,
	//TLB miss
	TLB_MISS,
	//TLB Multihit
	TLB_MHIT,
	//Mem is read/write protected (depends on translation type)
	PROTECTED,
	//Mem is write protected , firstwrite
	FIRSTWRITE,
	//data-Opcode read/write misaligned
	BADADDR
};

struct TLB_Entry
{
	CCN_PTEH_type Address;
	CCN_PTEL_type Data;
	CCN_PTEA_type Assistance;
};

extern TLB_Entry UTLB[64];
extern TLB_Entry ITLB[4];
extern bool mmuOn;

constexpr u32 fast_reg_lut[8] =
{
	0, 0, 0, 0	//P0-U0
	, 1		//P1
	, 1		//P2
	, 0		//P3
	, 1		//P4
};

constexpr u32 mmu_mask[4] =
{
	((0xFFFFFFFF) >> 10) << 10,	//1 kb page
	((0xFFFFFFFF) >> 12) << 12,	//4 kb page
	((0xFFFFFFFF) >> 16) << 16,	//64 kb page
	((0xFFFFFFFF) >> 20) << 20	//1 MB page
};

bool UTLB_Sync(u32 entry);
void ITLB_Sync(u32 entry);

bool mmu_match(u32 va, CCN_PTEH_type Address, CCN_PTEL_type Data);
void mmu_set_state();
void mmu_flush_table();
[[noreturn]] void mmu_raise_exception(MmuError mmu_error, u32 address, u32 am);

static inline bool mmu_enabled()
{
	return mmuOn;
}

// Deadcast: whether the block at from may jump straight to the block at to: when neither address can
// translate differently than when linked (both in untranslated P1/P2, or both in the same 4K user
// page: blocks never leave their page, and rdv_LinkBlock checks the target's physical page).
static inline bool mmu_canLink(u32 from, u32 to)
{
	if (fast_reg_lut[from >> 29] != 0 && fast_reg_lut[to >> 29] != 0)
		return true;
	return (from >> 31) == 0 && (to >> 31) == 0 && (from >> 12) == (to >> 12);
}

MmuError mmu_full_lookup(u32 va, const TLB_Entry **entry, u32& rv);
// Deadcast: the store queues' last translated page (mmu_full_SQ) per ASID: they write to the same
// place (the TA FIFO) vertex after vertex. Dropped when an entry for its page is cached (fastmmu.cpp).
struct SqPage
{
	u32 vpage = 1;		// odd: none
	u32 ppage;
};
extern SqPage sqPages[256];
bool mmu_peek(u32 va, u32& pa);
MmuError mmu_instruction_lookup(u32 va, const TLB_Entry **entry, u32& rv);
template<u32 translation_type>
MmuError mmu_full_SQ(u32 va, u32& rv);

#ifdef FAST_MMU
static inline MmuError mmu_instruction_translation(u32 va, u32& rv)
{
	if (fast_reg_lut[va >> 29] != 0)
	{
		rv = va;
		return MmuError::NONE;
	}

	return mmu_full_lookup(va, nullptr, rv);
}
#else
MmuError mmu_instruction_translation(u32 va, u32& rv);
#endif

template<u32 translation_type>
MmuError mmu_data_translation(u32 va, u32& rv);
void DoMMUException(u32 addr, MmuError mmu_error, u32 access_type);

inline static bool mmu_is_translated(u32 va, u32 size)
{
#ifndef FAST_MMU
	if (va & (std::min(size, 4u) - 1))
		return true;
#endif

	if (fast_reg_lut[va >> 29] != 0)
		return false;

	if ((va & 0xFC000000) == 0x7C000000)
		// On-chip RAM area isn't translated
		return false;

	return true;
}

template<typename T> T DYNACALL mmu_ReadMem(u32 adr);
u16 DYNACALL mmu_IReadMem16(u32 addr);

template<typename T> void DYNACALL mmu_WriteMem(u32 adr, T data);

void mmu_TranslateSQW(u32 adr, u32* out);
// the store queues' 1 MB pages, for the MMU on without full emulation (mmu_TranslateSQW)
extern u32 sq_remap[64];

#ifdef FAST_MMU
// Deadcast: by 4K virtual page, the page's host address in the memory map minus its virtual address
// (0 if not cached), so an access's host address is entry + vaddr (ldr/str [entry, vaddr] in the
// dynarec). The physical address is that minus the map's base.
extern uintptr_t mmuAddressLUT[0x100000];

static inline uintptr_t mmuHostPage(u32 vaddr, u32 paddr) {
	return (uintptr_t)addrspace::ram_base + (paddr & 0x1FFFF000) - (vaddr & ~0xfffu);
}

// Deadcast: recent blocks' code by user-space pc, looked up first by the dynarec's main loop (before
// mmuAddressLUT and the block table). Emptied with mmuAddressLUT and when blocks are discarded.
struct MmuDispatchEntry
{
	u32 vaddr;	// an odd address when empty: those are never looked up here
	u32 pad;
	void *code;
};
constexpr u32 MmuDispatchEntries = 4096;
extern MmuDispatchEntry mmuDispatchCache[MmuDispatchEntries];
void mmuDispatchCacheFlush();

// Deadcast: the entries of mmuDispatchCache and mmuAddressLUT slot 0 filled since the last flush, so a
// flush (each ASID change) empties only those, unless there were more than Max
struct MmuFilled
{
	static constexpr u32 Max = 1024;
	u32 count;
	u32 index[Max];
};
extern MmuFilled mmuDispatchFilled;
extern MmuFilled mmuSlot0Filled;
constexpr u32 MmuSlot0Pages = (32 * 1024 * 1024) >> 12;

// caches the page of vaddr in mmuAddressLUT
static inline void mmuCachePage(u32 vaddr, u32 paddr)
{
	const u32 vpn = vaddr >> 12;
	if (vpn < MmuSlot0Pages && mmuAddressLUT[vpn] == 0)
	{
		if (mmuSlot0Filled.count < MmuFilled::Max)
			mmuSlot0Filled.index[mmuSlot0Filled.count] = vpn;
		mmuSlot0Filled.count++;
	}
	mmuAddressLUT[vpn] = mmuHostPage(vaddr, paddr);
}

// Deadcast: counts for the perf line (spg.cpp): page cache flushes, full and partial (ASID changes),
// and lookups it missed (mmuDynarecLookup)
extern u32 mmuStatFullFlushes, mmuStatPartialFlushes, mmuStatMisses;

static inline void mmuAddressLUTFlush(bool full)
{
	if (full)
		mmuStatFullFlushes++;
	else
		mmuStatPartialFlushes++;
	// with the MMU windows, each ASID has a dispatch cache of its own
	if (full || mmuwin::current == nullptr)
		mmuDispatchCacheFlush();
	if (full)
	{
		memset(mmuAddressLUT, 0, sizeof(mmuAddressLUT) / 2);	// flush user memory
		mmuwin::flushAll();
	}
	else if (mmuSlot0Filled.count <= MmuFilled::Max)
	{
		// flush slot 0: the pages cached since the last flush
		for (u32 i = 0; i < mmuSlot0Filled.count; i++)
			mmuAddressLUT[mmuSlot0Filled.index[i]] = 0;
	}
	else
		memset(mmuAddressLUT, 0, MmuSlot0Pages * sizeof(mmuAddressLUT[0]));		// flush slot 0
	mmuSlot0Filled.count = 0;
}
#endif

#if FEAT_SHREC == DYNAREC_JIT
// The physical address of a memory access at vaddr, raising the exception if it has none, and its
// page cached in mmuAddressLUT: called by the dynarec's code when the page is not cached.
static inline u32 DYNACALL mmuDynarecLookup(u32 vaddr, u32 write, u32 pc)
{
	mmuStatMisses++;
	u32 paddr;
	MmuError rv;
	// TODO pass access size so that alignment errors are raised
	if (write)
		rv = mmu_data_translation<MMU_TT_DWRITE>(vaddr, paddr);
	else
		rv = mmu_data_translation<MMU_TT_DREAD>(vaddr, paddr);
	if (unlikely(rv != MmuError::NONE))
	{
		Sh4cntx.pc = pc;
		DoMMUException(vaddr, rv, write ? MMU_TT_DWRITE : MMU_TT_DREAD);
		host_context_t ctx;
		sh4Dynarec->handleException(ctx);
		((void (*)())ctx.pc)();
		// not reached
		return 0;
	}
#ifdef FAST_MMU
	// Deadcast: P1, P2 and P4 too, never translated: the generated code then finds them without calling
	// here (kernel code and data, the store queues). A full flush leaves them, which is right.
	if (vaddr >> 31 == 0 || fast_reg_lut[vaddr >> 29] != 0)
		mmuCachePage(vaddr, paddr);
#endif

	return paddr;
}

#endif

void MMU_init();
void MMU_reset();
void MMU_term();

void mmu_serialize(Serializer& ser);
void mmu_deserialize(Deserializer& deser);
