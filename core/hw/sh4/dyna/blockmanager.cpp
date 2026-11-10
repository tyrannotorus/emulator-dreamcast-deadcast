/*
	Tiny cute block manager. Doesn't keep block graphs or anything fancy ...
	Its based on a simple hashed-lists idea
*/

#include "deadcast/mmuwindows.h"
#include "deadcast/profiler.h"
#include <algorithm>
#include <set>
#include <map>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include "blockmanager.h"
#include "ngen.h"

#include "hw/sh4/sh4_core.h"
#include "hw/sh4/sh4_interrupts.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/sh4_opcode_list.h"
#include "hw/sh4/sh4_sched.h"
#include "hw/sh4/modules/mmu.h"
#include "oslib/virtmem.h"

#if defined(__unix__) && defined(DYNA_OPROF)
#include <opagent.h>
op_agent_t          oprofHandle;
#endif

#if FEAT_SHREC != DYNAREC_NONE


typedef std::vector<RuntimeBlockInfoPtr> bm_List;
typedef std::set<RuntimeBlockInfoPtr> bm_Set;
typedef std::map<void*, RuntimeBlockInfoPtr> bm_Map;

static bm_Set all_temp_blocks;
static bm_List del_blocks;

static u32 pageCount;
bool *unprotected_pages;
static std::set<RuntimeBlockInfo*> *blocks_per_page;

static bm_Map blkmap;
// Stats
u32 protected_blocks;
u32 unprotected_blocks;

// Deadcast: without the MMU, a write to a protected page of code leaves the page protected (upstream unprotected it
// for good, and every block compiled there checked its code on each entry).
// - A store of translated code through the window goes to its slow path for good (Sh4Dynarec::storeToStub), which
//   writes past the protection and throws away the blocks whose code or constants it changed (bm_CodeWrite).
// - Any other write throws the page's blocks away and unlocks it; the next block compiled there locks it again.
// - A word read into a constant (constReads) that is written is data from then on: never read into a constant again.
namespace
{
std::unordered_set<u32> dataWords;		// RAM words written as data next to code
// RAM page -> a bit for each byte a protected block was compiled from or read into a constant (bm_CodeWrite's test)
std::vector<std::unique_ptr<u64[]>> codeBits;

void markCodeBytes(u32 offset, u32 size)
{
	offset &= RAM_MASK;
	while (size > 0)
	{
		const u32 page = offset / PAGE_SIZE;
		const u32 from = offset % PAGE_SIZE;
		const u32 n = std::min(size, (u32)PAGE_SIZE - from);
		std::unique_ptr<u64[]>& bits = codeBits[page];
		if (!bits)
			bits.reset(new u64[PAGE_SIZE / 64]());
		for (u32 i = from; i < from + n; i++)
			bits[i / 64] |= 1ull << (i % 64);
		offset += n;
		size -= n;
	}
}

// within one page
bool codeBytes(u32 offset, u32 size)
{
	const std::unique_ptr<u64[]>& bits = codeBits[offset / PAGE_SIZE];
	if (!bits)
		return false;
	for (u32 i = offset % PAGE_SIZE; i < offset % PAGE_SIZE + size; i++)
		if ((bits[i / 64] >> (i % 64)) & 1)
			return true;
	return false;
}
}

#define FPCA(x) ((DynarecCodeEntryPtr&)p_sh4rcb->fpcb[(x>>1)&FPCB_MASK])

// addr must be a physical address
// This returns an executable address
static DynarecCodeEntryPtr DYNACALL bm_GetCode(u32 addr)
{
	DynarecCodeEntryPtr rv = FPCA(addr);

	return rv;
}

// addr must be a virtual address
// This returns an executable address
DynarecCodeEntryPtr DYNACALL bm_GetCodeByVAddr(u32 addr)
{
	if (!mmu_enabled())
		return bm_GetCode(addr);

	if (addr & 1)
	{
		switch (addr)
		{
#ifdef USE_WINCE_HACK
		case 0xfffffde7: // GetTickCount
			// This should make this syscall faster
			Sh4cntx.r[0] = sh4_sched_now64() * 1000 / SH4_MAIN_CLOCK;
			Sh4cntx.pc = Sh4cntx.pr;
			Sh4cntx.cycle_counter -= 100;
			break;

		case 0xfffffd05: // QueryPerformanceCounter(u64 *)
			{
				bool isRam;
				u64 *ptr;
				u32 paddr;
				if (rdv_writeMemImmediate(Sh4cntx.r[4], sizeof(u64), (void*&)ptr, isRam, paddr) && isRam)
				{
					*ptr = sh4_sched_now64() >> 4;
					Sh4cntx.r[0] = 1;
					Sh4cntx.pc = Sh4cntx.pr;
					Sh4cntx.cycle_counter -= 100;
				}
				else
				{
					Do_Exception(addr, Sh4Ex_AddressErrorRead);
				}
			}
			break;
		case 0xfffffded: // GetProcAddressW
			{
				// Intercept the dricas authentication function and replace it with a stub (Hundred Swords, Rune Jade)
				static constexpr u16 Function[] = { 'U', 'A', '_', 'Q', 'u', 'e', 'r', 'y', 'D', '\0' };
				bool success = false;
				u32 vaddr = Sh4cntx.r[5];
				if ((vaddr & 0xffff0000) != 0) // not an ordinal
				{
					u32 pa = 0;
					for (unsigned i = 0; ; i++)
					{
						if (pa == 0 && mmu_full_lookup(vaddr, nullptr, pa) != MmuError::NONE)
							break;
						u16 c = ReadMem16_nommu(pa);
						if (c != Function[i])
							break;
						if (c == 0) {
							success = true;
							break;
						}
						vaddr += 2;
						if ((vaddr & 0xfff) == 0)
							pa = 0;
						else
							pa += 2;
					}
				}
				if (success)
				{
					INFO_LOG(DYNAREC, "GetProcAddressW('UA_QueryD') intercepted");
					Sh4cntx.r[0] = 0xffff0001;
					Sh4cntx.pc = Sh4cntx.pr;
				}
				else {
					Do_Exception(addr, Sh4Ex_AddressErrorRead);
				}
			}
			break;
		case 0xffff0001:	// UA_QueryD stub
			DEBUG_LOG(DYNAREC, "UA_QueryD called");
			Sh4cntx.r[0] = 0;
			Sh4cntx.pc = Sh4cntx.pr;
			break;
#endif

		default:
			Do_Exception(addr, Sh4Ex_AddressErrorRead);
			break;
		}
		addr = Sh4cntx.pc;
	}

	u32 paddr;
	MmuError rv = mmu_instruction_translation(addr, paddr);
	if (rv != MmuError::NONE)
	{
		DoMMUException(addr, rv, MMU_TT_IREAD);
		addr = Sh4cntx.pc;
		rv = mmu_instruction_translation(addr, paddr);
	}
#ifdef FAST_MMU
	// Deadcast: cache the user page as the memory accesses do (mmuDynarecLookup), for the dynarec's
	// main loop to find the next block's code without calling here
	if (rv == MmuError::NONE && (addr >> 31) == 0)
	{
		// Deadcast: slot 0's in the window's own table, kept across ASID changes (deadcast/mmuwindows.h)
		uintptr_t *codeLut = mmuwin::codeLut();
		if (codeLut != nullptr && (addr >> 12) < MmuSlot0Pages)
			codeLut[addr >> 12] = mmuHostPage(addr, paddr);
		else
			mmuCachePage(addr, paddr);
	}
#endif

	return bm_GetCode(paddr);
}

// addr must be a physical address
// This returns an executable address
RuntimeBlockInfoPtr DYNACALL bm_GetBlock(u32 addr)
{
	DynarecCodeEntryPtr cde = bm_GetCode(addr);  // Returns RX ptr

	if (cde == ngen_FailedToFindBlock)
		return NULL;
	else
		return bm_GetBlock((void*)cde);  // Returns RX pointer
}

// This takes a RX address and returns the info block ptr (RW space)
RuntimeBlockInfoPtr bm_GetBlock(void* dynarec_code)
{
	if (blkmap.empty())
		return NULL;

	void *dynarecrw = CC_RX2RW(dynarec_code);
	// Returns a block who's code addr is bigger than dynarec_code (or end)
	auto iter = blkmap.upper_bound(dynarecrw);
	if (iter == blkmap.begin())
		return NULL;
	iter--;  // Need to go back to find the potential candidate

	// However it might be out of bounds, check for that
	if (!iter->second->containsCode(dynarecrw))
		return NULL;

	return iter->second;
}

static void bm_CleanupDeletedBlocks()
{
	del_blocks.clear();
}

// Takes RX pointer and returns a RW pointer
RuntimeBlockInfoPtr bm_GetStaleBlock(void* dynarec_code)
{
	void *dynarecrw = CC_RX2RW(dynarec_code);
	if (del_blocks.empty())
		return NULL;
	// Start from the end to get the youngest one
	auto it = del_blocks.end();
	do
	{
		--it;
		if ((*it)->containsCode(dynarecrw))
			return *it;
	} while (it != del_blocks.begin());

	return NULL;
}

std::vector<RuntimeBlockInfoPtr> bm_AllBlocks()
{
	std::vector<RuntimeBlockInfoPtr> blocks;
	for (const auto& [code, block] : blkmap)
		blocks.push_back(block);
	blocks.insert(blocks.end(), del_blocks.begin(), del_blocks.end());
	return blocks;
}

u32 bmStatCompiled, bmStatCacheResets;	// Deadcast: for the perf line (spg.cpp)
u32 bmStatCodeWrites, bmStatCodeDropped, bmStatCodeStores;

void bm_AddBlock(RuntimeBlockInfo* blk)
{
	bmStatCompiled++;
	RuntimeBlockInfoPtr block(blk);
	if (block->temp_block)
		all_temp_blocks.insert(block);
	auto iter = blkmap.find((void*)blk->code);
	if (iter != blkmap.end()) {
		ERROR_LOG(DYNAREC, "DUP: %08X %p %08X %p", iter->second->addr, iter->second->code, block->addr, block->code);
		die("Duplicated block");
	}
	blkmap[(void*)block->code] = block;

	verify((void*)bm_GetCode(block->addr) == (void*)ngen_FailedToFindBlock);
	FPCA(block->addr) = (DynarecCodeEntryPtr)CC_RW2RX(block->code);
	// Deadcast: what a write must not change without throwing it away (bm_CodeWrite)
	if (block->read_only)
	{
		markCodeBytes(block->addr, block->sh4_code_size);
		for (const auto& [offset, size] : block->constReads)
			markCodeBytes(offset, size);
	}

#ifdef DYNA_OPROF
	if (oprofHandle)
	{
		char fname[512];

		snprintf(fname, sizeof(fname), "sh4:%08X,c:%d,s:%d,h:%d", block->addr, block->guest_cycles, block->guest_opcodes, block->host_opcodes);

		if (op_write_native_code(oprofHandle, fname, (uint64_t)block->code, (void*)block->code, block->host_code_size) != 0)
		{
			INFO_LOG(DYNAREC, "op_write_native_code error");
		}
	}
#endif

}

void bm_ForEachBlock(const std::function<void(const RuntimeBlockInfo&)>& fn)
{
	for (const auto& it : blkmap)
		fn(*it.second);
}

bool bm_HasBlock(u32 addr)
{
	return (void*)bm_GetCode(addr) != (void*)ngen_FailedToFindBlock;
}

void bm_DiscardBlock(RuntimeBlockInfo* block)
{
	// Remove from block map
	auto it = blkmap.find((void*)block->code);
	verify(it != blkmap.end());
	RuntimeBlockInfoPtr block_ptr = it->second;

	blkmap.erase(it);

	block_ptr->pNextBlock = NULL;
	block_ptr->pBranchBlock = NULL;
	block_ptr->Relink();

	// Remove from jump table
	verify((void*)bm_GetCode(block_ptr->addr) == CC_RW2RX((void*)block_ptr->code));
	FPCA(block_ptr->addr) = ngen_FailedToFindBlock;
#ifdef FAST_MMU
	mmuDispatchCacheFlush();
#endif

	if (block_ptr->temp_block)
		all_temp_blocks.erase(block_ptr);

	del_blocks.push_back(block_ptr);
	block_ptr->Discard();
}

void bm_Periodical_1s()
{
	bm_CleanupDeletedBlocks();
}

void bm_vmem_pagefill(void** ptr, u32 size_bytes)
{
#ifdef FAST_MMU
	mmuDispatchCacheFlush();
#endif
	for (size_t i = 0; i < size_bytes / sizeof(ptr[0]); i++)
	{
		ptr[i]=(void*)ngen_FailedToFindBlock;
	}
}

void bm_Reset()
{
	bm_CleanupDeletedBlocks();
	mmuwin::unprotectAll();
	if (bm_codeLocked != nullptr)
		memset(bm_codeLocked, 0, pageCount);
	protected_blocks = 0;
	unprotected_blocks = 0;

#ifndef __SWITCH__
	if (addrspace::virtmemEnabled())
	{
		// Windows cannot lock/unlock a region spanning more than one VirtualAlloc or MapViewOfFile
		// so we have to unlock each region individually
		if (settings.platform.ram_size == 16_MB)
		{
			virtmem::region_unlock(addrspace::ram_base + 0x0C000000, RAM_SIZE);
			virtmem::region_unlock(addrspace::ram_base + 0x0D000000, RAM_SIZE);
			virtmem::region_unlock(addrspace::ram_base + 0x0E000000, RAM_SIZE);
			virtmem::region_unlock(addrspace::ram_base + 0x0F000000, RAM_SIZE);
		}
		else
		{
			virtmem::region_unlock(addrspace::ram_base + 0x0C000000, RAM_SIZE);
			virtmem::region_unlock(addrspace::ram_base + 0x0E000000, RAM_SIZE);
		}
		addrspace::protectRamInWindow(0, RAM_SIZE, false);
	}
	else
#endif
	{
		virtmem::region_unlock(&mem_b[0], RAM_SIZE);
	}
}

u8 *bm_codeLocked;

void bm_LockPage(u32 addr, u32 size)
{
	addr = addr & (RAM_MASK - PAGE_MASK);
	// Deadcast: stores that reach it through addrspace::writet go past the lock (bm_CodeWrite), through the RAM's
	// next view, which only virtmem has
	if (addrspace::virtmemEnabled())
		for (u32 offset = 0; offset < size; offset += PAGE_SIZE)
			bm_codeLocked[(addr + offset) / PAGE_SIZE] = 1;
	// Deadcast: the page's mappings in the MMU windows too (deadcast/mmuwindows.h)
	for (u32 offset = 0; offset < size; offset += PAGE_SIZE)
		mmuwin::protectRamPage(addr + offset, true);
	if (addrspace::virtmemEnabled())
	{
		virtmem::region_lock(addrspace::ram_base + 0x0C000000 + addr, size);
		addrspace::protectRamInWindow(addr, size, true);
	}
	else
		virtmem::region_lock(&mem_b[addr], size);
}

void bm_UnlockPage(u32 addr, u32 size)
{
	addr = addr & (RAM_MASK - PAGE_MASK);
	for (u32 offset = 0; offset < size; offset += PAGE_SIZE)
		bm_codeLocked[(addr + offset) / PAGE_SIZE] = 0;
	for (u32 offset = 0; offset < size; offset += PAGE_SIZE)
		mmuwin::protectRamPage(addr + offset, false);
	if (addrspace::virtmemEnabled())
	{
		virtmem::region_unlock(addrspace::ram_base + 0x0C000000 + addr, size);
		addrspace::protectRamInWindow(addr, size, false);
	}
	else
		virtmem::region_unlock(&mem_b[addr], size);
}

// Deadcast: code a game rewrites in place again and again with the same few versions (Armada writes its
// vertex routines into two buffers, a few versions draw after draw, hundreds of times a second): each version
// is compiled once, as any block, kept (parked) while another version is there, and put back when its code
// is there again, instead of compiled again, unoptimised (a temp block), every time. At most MaxParked
// versions an address, the oldest dropped; an address compiled WildCompiles times goes to temp blocks.
namespace
{
struct SmcAddress
{
	std::deque<RuntimeBlockInfoPtr> parked;
	u32 compiles = 0;
};
std::unordered_map<u32, SmcAddress> smcAddresses;
constexpr size_t MaxParked = 8;
constexpr u32 WildCompiles = 64;
}

bool bm_SmcVariantsUsable(u32 addr)
{
	auto it = smcAddresses.find(addr);
	return it == smcAddresses.end() || it->second.compiles < WildCompiles;
}

void bm_SmcVariantCompiled(RuntimeBlockInfo* block)
{
	const u8 *code = GetMemPtr(block->addr, block->sh4_code_size);
	if (code == nullptr)
		return;
	block->smcCode.assign(code, code + block->sh4_code_size);
	smcAddresses[block->addr].compiles++;
}

// as bm_DiscardBlock, but kept for bm_UnparkBlock
void bm_ParkBlock(RuntimeBlockInfo* block)
{
	auto it = blkmap.find((void*)block->code);
	verify(it != blkmap.end());
	RuntimeBlockInfoPtr block_ptr = it->second;
	blkmap.erase(it);

	block_ptr->pNextBlock = NULL;
	block_ptr->pBranchBlock = NULL;
	block_ptr->Relink();

	verify((void*)bm_GetCode(block_ptr->addr) == CC_RW2RX((void*)block_ptr->code));
	FPCA(block_ptr->addr) = ngen_FailedToFindBlock;
#ifdef FAST_MMU
	mmuDispatchCacheFlush();
#endif
	// the blocks linked to it go through the main loop
	block_ptr->Discard();

	auto& parked = smcAddresses[block_ptr->addr].parked;
	parked.push_back(block_ptr);
	if (parked.size() > MaxParked)
	{
		del_blocks.push_back(parked.front());
		parked.pop_front();
	}
}

// the version of the code at addr (physical) compiled for vaddr, if parked and that code is there again
DynarecCodeEntryPtr bm_UnparkBlock(u32 addr, u32 vaddr, fpscr_t fpu)
{
	auto it = smcAddresses.find(addr);
	if (it == smcAddresses.end())
		return nullptr;
	auto& parked = it->second.parked;
	for (auto p = parked.begin(); p != parked.end(); ++p)
	{
		RuntimeBlockInfoPtr block = *p;
		// what the decoder compiled it for (state_Setup)
		if (block->vaddr != vaddr || block->fpu_cfg.PR != fpu.PR || block->fpu_cfg.SZ != fpu.SZ
				|| block->fpu_cfg.RM != fpu.RM)
			continue;
		const u8 *code = GetMemPtr(addr, block->sh4_code_size);
		if (code == nullptr || memcmp(code, block->smcCode.data(), block->sh4_code_size) != 0)
			continue;
		parked.erase(p);
		verify((void*)bm_GetCode(addr) == (void*)ngen_FailedToFindBlock);
		blkmap[(void*)block->code] = block;
		FPCA(addr) = (DynarecCodeEntryPtr)CC_RW2RX(block->code);
#ifdef FAST_MMU
		mmuDispatchCacheFlush();
#endif
		return block->code;
	}
	return nullptr;
}

void bm_ResetCache()
{
#ifdef FAST_MMU
	mmuDispatchCacheFlush();
#endif
	sh4Dynarec->reset();
	addrspace::bm_reset();

	for (const auto& it : blkmap)
	{
		RuntimeBlockInfoPtr block = it.second;
		block->relink_data = 0;
		block->pNextBlock = NULL;
		block->pBranchBlock = NULL;
		// needed for the transition to full mmu. Could perhaps limit it to the current block.
		block->Relink();
		// Avoid circular references
		block->Discard();
		del_blocks.push_back(block);
	}

	blkmap.clear();
	// Deadcast: the parked versions' code is gone too
	for (auto& [addr, smc] : smcAddresses)
		del_blocks.insert(del_blocks.end(), smc.parked.begin(), smc.parked.end());
	smcAddresses.clear();
	// blkmap includes temp blocks as well
	all_temp_blocks.clear();

	for (size_t i = 0; i < pageCount; i++)
		blocks_per_page[i].clear();
	for (auto& bits : codeBits)
		bits.reset();

	memset(unprotected_pages, 0, pageCount);
	dataWords.clear();

#ifdef DYNA_OPROF
	if (oprofHandle)
	{
		for (int i=0;i<del_blocks.size();i++)
		{
			if (op_unload_native_code(oprofHandle, (uint64_t)del_blocks[i]->code) != 0)
			{
				INFO_LOG(DYNAREC, "op_unload_native_code error");
			}
		}
	}
#endif
}

void bm_ResetTempCache(bool full)
{
#ifdef FAST_MMU
	mmuDispatchCacheFlush();
#endif
	if (!full)
	{
		for (const auto& block : all_temp_blocks)
		{
			FPCA(block->addr) = ngen_FailedToFindBlock;
			blkmap.erase((void*)block->code);
		}
	}
	del_blocks.insert(del_blocks.begin(),all_temp_blocks.begin(),all_temp_blocks.end());
	all_temp_blocks.clear();
}

void bm_Init()
{
	pageCount = RAM_SIZE_MAX / PAGE_SIZE;
	unprotected_pages = new bool[pageCount];
	blocks_per_page = new std::set<RuntimeBlockInfo*>[pageCount];
	bm_codeLocked = new u8[pageCount]();
	codeBits.resize(pageCount);

#ifdef DYNA_OPROF
	oprofHandle=op_open_agent();
	if (oprofHandle==0)
		INFO_LOG(DYNAREC, "bm: Failed to open oprofile");
	else
		INFO_LOG(DYNAREC, "bm: Oprofile integration enabled !");
#endif
}

void bm_Term()
{
#ifdef DYNA_OPROF
	if (oprofHandle) op_close_agent(oprofHandle);
	
	oprofHandle=0;
#endif
	bm_Reset();
	delete[] unprotected_pages;
	delete[] blocks_per_page;
	delete[] bm_codeLocked;
	bm_codeLocked = nullptr;
}

void bm_WriteBlockMap(const std::string& file)
{
	FILE* f=fopen(file.c_str(),"wb");
	if (f)
	{
		INFO_LOG(DYNAREC, "Writing block map !");
		for (const auto& [_, block] : blkmap)
		{
			fprintf(f, "block: %d:%08X:%p:%d:%d:%d\n", block->BlockType, block->addr, block->code, block->host_code_size, block->guest_cycles, block->guest_opcodes);
			for(size_t j = 0; j < block->oplist.size(); j++)
				fprintf(f,"\top: %zd:%d:%s\n", j, block->oplist[j].guest_offs, block->oplist[j].dissasm().c_str());
		}
		fclose(f);
		INFO_LOG(DYNAREC, "Finished writing block map");
	}
}

void sh4_jitsym(FILE* out)
{
	for (const auto& [_, block] : blkmap)
	{
		fprintf(out, "%p %d %08X\n", block->code, block->host_code_size, block->addr);
	}
}

RuntimeBlockInfo::~RuntimeBlockInfo()
{
	// Deadcast: how many times blocks ran before they went (profiling: deadcast/profiler.h)
	profilerBlockGone(runs);
	if (sh4_code_size != 0)
	{
		if (read_only)
			protected_blocks--;
		else
			unprotected_blocks--;
	}
}

void RuntimeBlockInfo::AddRef(const RuntimeBlockInfoPtr& other)
{ 
	pre_refs.push_back(other); 
}

void RuntimeBlockInfo::RemRef(const RuntimeBlockInfoPtr& other)
{
	auto it = std::find(pre_refs.begin(), pre_refs.end(), other);
	if (it != pre_refs.end())
		pre_refs.erase(it);
}

void RuntimeBlockInfo::Discard()
{
	// Update references
	for (RuntimeBlockInfoPtr& ref : pre_refs)
	{
		if (ref->pNextBlock == this)
			ref->pNextBlock = nullptr;
		if (ref->pBranchBlock == this)
			ref->pBranchBlock = nullptr;
		ref->relink_data = 0;
		ref->Relink();
	}
	pre_refs.clear();

	if (read_only)
	{
		// Remove this block from the per-page block lists
		for (u32 addr = this->addr & ~PAGE_MASK; addr < this->addr + this->sh4_code_size; addr += PAGE_SIZE)
		{
			auto& block_list = blocks_per_page[(addr & RAM_MASK) / PAGE_SIZE];
			block_list.erase(this);
			if (block_list.empty())
				codeBits[(addr & RAM_MASK) / PAGE_SIZE].reset();
		}
	}
}

void RuntimeBlockInfo::SetProtectedFlags()
{
	// Don't write protect rom and BIOS/IP.BIN (Grandia II)
	if (!IsOnRam(addr) || (addr & 0x1FFF0000) == 0x0c000000)
	{
		this->read_only = false;
		unprotected_blocks++;
		return;
	}
	for (u32 addr = this->addr & ~PAGE_MASK; addr < this->addr + sh4_code_size; addr += PAGE_SIZE)
	{
		if (unprotected_pages[(addr & RAM_MASK) / PAGE_SIZE])
		{
			this->read_only = false;
			unprotected_blocks++;
			return;
		}
	}
	this->read_only = true;
	protected_blocks++;
	for (u32 addr = this->addr & ~PAGE_MASK; addr < this->addr + sh4_code_size; addr += PAGE_SIZE)
	{
		auto& block_list = blocks_per_page[(addr & RAM_MASK) / PAGE_SIZE];
		if (block_list.empty())
			bm_LockPage(addr);
		block_list.insert(this);
	}
}

bool bm_IsDataWord(u32 offset)
{
	return !dataWords.empty() && dataWords.count((offset & RAM_MASK) & ~3u) != 0;
}

bool bm_StoreToSlowPath(void *p)
{
	if (mmu_enabled() || !addrspace::virtmemEnabled())
		return false;
	const u32 offset = bm_getRamOffset(p);
	return offset != (u32)-1 && !unprotected_pages[offset / PAGE_SIZE];
}

void bm_CodeWrite(u32 offset, u32 size, u64 data)
{
	offset &= RAM_MASK;
	bmStatCodeStores++;
	if (memcmp(&mem_b[offset], &data, size) == 0)
		return;
	// through the RAM's next view (its first mirror), never locked
	memcpy(&mem_b[RAM_SIZE + offset], &data, size);
	if (!codeBytes(offset, size))
		return;
	std::set<RuntimeBlockInfo*>& block_list = blocks_per_page[offset / PAGE_SIZE];
	const auto touched = [&](u32 addr, u32 bytes) { return addr < offset + size && addr + bytes > offset; };
	std::vector<RuntimeBlockInfo*> stale;
	for (RuntimeBlockInfo *block : block_list)
	{
		bool hit = touched(block->addr & RAM_MASK, block->sh4_code_size);
		for (const auto& [addr, bytes] : block->constReads)
			if (touched(addr, bytes))
			{
				dataWords.insert(addr & ~3u);
				hit = true;
			}
		if (hit)
			stale.push_back(block);
	}
	for (RuntimeBlockInfo *block : stale)
		bm_DiscardBlock(block);
	bmStatCodeDropped += stale.size();
}

void bm_RamWriteAccess(u32 addr)
{
	addr &= RAM_MASK;
	if (unprotected_pages[addr / PAGE_SIZE])
	{
		// Deadcast: should an alias of it in the dynarec's window still be locked, not again
		addrspace::protectRamInWindow(addr & ~PAGE_MASK, PAGE_SIZE, false);
		return;
	}
	// Deadcast: without the MMU, the next block compiled there locks the page again (SetProtectedFlags)
	if (mmu_enabled())
		unprotected_pages[addr / PAGE_SIZE] = true;
	else
		bmStatCodeWrites++;
	bm_UnlockPage(addr);
	std::set<RuntimeBlockInfo*>& block_list = blocks_per_page[addr / PAGE_SIZE];
	if (!block_list.empty())
	{
		std::vector<RuntimeBlockInfo*> list_copy;
		list_copy.insert(list_copy.begin(), block_list.begin(), block_list.end());
		if (!list_copy.empty())
			DEBUG_LOG(DYNAREC, "bm_RamWriteAccess write access to %08x pc %08x", addr, Sh4cntx.pc);
		bmStatCodeDropped += list_copy.size();
		for (auto& block : list_copy)
			bm_DiscardBlock(block);
		verify(block_list.empty());
	}
}

u32 bm_getRamOffset(void *p)
{
#ifndef __SWITCH__
	if (addrspace::virtmemEnabled())
	{
		if ((u8 *)p < addrspace::ram_base || (u8 *)p >= addrspace::ram_base + 0x20000000)
			return -1;
		u32 addr = (u8*)p - addrspace::ram_base;
		if (!IsOnRam(addr))
			return -1;
		return addr & RAM_MASK;
	}
	else
#endif
	{
		uintptr_t offset = virtmem::untag(p) - virtmem::untag(&mem_b[0]);
		if (offset >= RAM_SIZE)
			return -1;
		return (u32)offset;
	}
}

bool bm_RamWriteAccess(void *p)
{
	u32 offset = bm_getRamOffset(p);
	if (offset == (u32)-1)
		return false;
	bm_RamWriteAccess(offset);
	return true;
}

bool print_stats = true;

void fprint_hex(FILE* d,const char* init,u8* ptr, u32& ofs, u32 limit)
{
	int base=ofs;
	int cnt=0;
	while(ofs<limit)
	{
		if (cnt==32)
		{
			fputs("\n",d);
			cnt=0;
		}

		if (cnt==0)
			fprintf(d,"%s:%d:",init,ofs-base);

		fprintf(d," %02X",ptr[ofs++]);
		cnt++;
	}
	fputs("\n",d);
}



void print_blocks()
{
	FILE* f=0;

	if (print_stats)
	{
		f=fopen(get_writable_data_path("blkmap.lst").c_str(),"w");
		print_stats=false;

		INFO_LOG(DYNAREC, "Writing blocks to %p", f);
	}

	for (const auto& [_, blk] : blkmap)
	{
		if (f)
		{
			fprintf(f,"block: %p\n",blk.get());
			fprintf(f,"vaddr: %08X\n",blk->vaddr);
			fprintf(f,"paddr: %08X\n",blk->addr);
			fprintf(f,"code: %p\n",blk->code);
			fprintf(f,"BlockType: %d\n",blk->BlockType);
			fprintf(f,"NextBlock: %08X\n",blk->NextBlock);
			fprintf(f,"BranchBlock: %08X\n",blk->BranchBlock);
			fprintf(f,"pNextBlock: %p\n",blk->pNextBlock);
			fprintf(f,"pBranchBlock: %p\n",blk->pBranchBlock);
			fprintf(f,"guest_cycles: %d\n",blk->guest_cycles);
			fprintf(f,"guest_opcodes: %d\n",blk->guest_opcodes);
			fprintf(f,"host_opcodes: %d\n",blk->host_opcodes);
			fprintf(f,"il_opcodes: %zd\n",blk->oplist.size());

			s32 gcode=-1;

			size_t j=0;
			
			fprintf(f,"{\n");
			for (;j<blk->oplist.size();j++)
			{
				shil_opcode* op = &blk->oplist[j];
				//fprint_hex(f,"//h:",pucode,hcode,op->host_offs);

				if (gcode!=op->guest_offs)
				{
					gcode=op->guest_offs;
					u32 rpc=blk->vaddr+gcode;
					try {
						u16 op=IReadMem16(rpc);

						char temp[128];
						OpDesc[op]->Disassemble(temp,rpc,op);

						fprintf(f,"//g: %04X %s\n", op, temp);
					} catch (const SH4ThrownException& ex) {
						fprintf(f,"//g: ???? (page fault)\n");
					}
				}

				std::string s = op->dissasm();
				fprintf(f,"//il:%d:%d: %s\n",op->guest_offs,op->host_offs,s.c_str());
			}
			
			//fprint_hex(f,"//h:",pucode,hcode,blk->host_code_size);

			fprintf(f,"}\n");
		}
	}

	if (f) fclose(f);
}
#endif

