#pragma once
#include <functional>

#include "types.h"
#include "decoder.h"
#include "shil.h"
#include "stdclass.h"

#include <memory>

typedef void (*DynarecCodeEntryPtr)();
struct RuntimeBlockInfo;
typedef std::shared_ptr<RuntimeBlockInfo> RuntimeBlockInfoPtr;

struct RuntimeBlockInfo
{
	bool Setup(u32 pc,fpscr_t fpu_cfg);

	u32 addr;
	u32 vaddr;
	DynarecCodeEntryPtr code;

	u32 host_code_size;	//in bytes
	u32 sh4_code_size; //in bytes

	fpscr_t fpu_cfg;
	u32 guest_cycles;
	u32 guest_opcodes;
	u32 host_opcodes;	// set by host code generator, optional
	bool has_fpu_op;
	bool temp_block;
	u32 blockcheck_failures;

	u32 BranchBlock; //if not 0xFFFFFFFF then jump target
	u32 NextBlock;   //if not 0xFFFFFFFF then next block (by position)

	//0 if not available
	RuntimeBlockInfo* pBranchBlock;
	RuntimeBlockInfo* pNextBlock; 

	u32 relink_offset;
	u32 relink_data;
	u64 runs = 0;	// Deadcast: times run, counted when profiling (deadcast/profiler.h)
	// Deadcast: under the MMU, the bytes of the pc check its code starts with, which a linked exit to
	// its vaddr enters past
	u32 pcCheckSize = 0;
	// Deadcast: under the MMU, each access through the window (offset in the code) and its stub's
	std::vector<std::pair<u32, u32>> mmuStubs;
	bool mmuNoLink = false;
	// Deadcast: the code it was compiled from, when the code at its address keeps being rewritten (bm_ParkBlock)
	std::vector<u8> smcCode;	// Deadcast: rdv_LinkBlock found an exit could not be linked under the MMU

	BlockEndType BlockType;
	bool has_jcond;
	bool read_only;
	// Deadcast: a conditional exit tests T where it is pinned (w18), not a copy of it in w29 (rec_arm64.cpp)
	bool exitTestsT = false;
	// Deadcast: the RAM it read at compile time into constants (ssa.cpp), offset and size: its code too
	// (bm_RamWriteAccess)
	std::vector<std::pair<u32, u32>> constReads;

	std::vector<shil_opcode> oplist;
	//predecessors references
	std::vector<RuntimeBlockInfoPtr> pre_refs;

	bool containsCode(const void *ptr)
	{
		return (u32)((const u8 *)ptr - (const u8 *)code) < host_code_size;
	}

	virtual ~RuntimeBlockInfo();

	virtual u32 Relink() {
		return 0;
	}
	
	void AddRef(const RuntimeBlockInfoPtr& other);
	void RemRef(const RuntimeBlockInfoPtr& other);

	void Discard();
	void SetProtectedFlags();
};

void bm_WriteBlockMap(const std::string& file);

DynarecCodeEntryPtr DYNACALL bm_GetCodeByVAddr(u32 addr);
RuntimeBlockInfoPtr bm_GetBlock(void* dynarec_code);
RuntimeBlockInfoPtr bm_GetStaleBlock(void* dynarec_code);
// Deadcast: every block, current and discarded since the last cleanup (the profiler)
std::vector<RuntimeBlockInfoPtr> bm_AllBlocks();
RuntimeBlockInfoPtr DYNACALL bm_GetBlock(u32 addr);

void bm_AddBlock(RuntimeBlockInfo* blk);
// Deadcast: every block in the cache (deadcast/codecache.cpp)
void bm_ForEachBlock(const std::function<void(const RuntimeBlockInfo&)>& fn);
// whether a block is compiled for this address
bool bm_HasBlock(u32 addr);
void bm_DiscardBlock(RuntimeBlockInfo* block);
// Deadcast: code rewritten in place with the same few versions over and over (blockmanager.cpp)
bool bm_SmcVariantsUsable(u32 addr);
void bm_SmcVariantCompiled(RuntimeBlockInfo* block);
void bm_ParkBlock(RuntimeBlockInfo* block);
DynarecCodeEntryPtr bm_UnparkBlock(u32 addr, u32 vaddr, fpscr_t fpu);
void bm_Reset();
void bm_ResetCache();
void bm_ResetTempCache(bool full);
void bm_Periodical_1s();

void bm_Init();
void bm_Term();

void bm_vmem_pagefill(void** ptr,u32 size_bytes);
static inline bool bm_IsRamPageProtected(u32 addr)
{
	extern bool *unprotected_pages;
	addr &= RAM_MASK;
	return !unprotected_pages[addr / PAGE_SIZE];
}

#if FEAT_SHREC != DYNAREC_NONE

// Deadcast: hostPc, the code that wrote (bm_StoreToSlowPath)
bool bm_RamWriteAccess(void *p, uintptr_t hostPc = 0);
void bm_RamWriteAccess(u32 addr, uintptr_t hostPc = 0);
// Deadcast: whether the dynarec's store at hostPc, faulting on protected code at p, goes to its slow path for good
// (Sh4Dynarec::storeToStub), where it writes past the protection (bm_CodeWrite)
bool bm_StoreToSlowPath(void *p, uintptr_t hostPc);
// Deadcast: a store to a page whose code is protected, past the protection: written through another view of the RAM,
// and the blocks whose code or constants it changed thrown away (addrspace::writet)
void bm_CodeWrite(u32 offset, u32 size, u64 data);
// Deadcast: a word of RAM next to code that is written as data, never read into a constant (ssa.cpp)
bool bm_IsDataWord(u32 offset);
extern u8 *bm_codeLocked;
static inline bool bm_IsCodeLocked(u32 offset) {
	return bm_codeLocked != nullptr && bm_codeLocked[(offset & RAM_MASK) / PAGE_SIZE] != 0;
}
void bm_LockPage(u32 addr, u32 size = PAGE_SIZE);
void bm_UnlockPage(u32 addr, u32 size = PAGE_SIZE);
u32 bm_getRamOffset(void *p);

#else

inline static bool bm_RamWriteAccess(void *p, uintptr_t hostPc = 0) {
	return false;
}
inline static void bm_RamWriteAccess(u32 addr, uintptr_t hostPc = 0) {}
inline static bool bm_StoreToSlowPath(void *p, uintptr_t hostPc) { return false; }
inline static void bm_CodeWrite(u32 offset, u32 size, u64 data) {}
inline static bool bm_IsDataWord(u32 offset) { return false; }
static inline bool bm_IsCodeLocked(u32 offset) { return false; }
inline static void bm_LockPage(u32 addr, u32 size = PAGE_SIZE) {}
inline static void bm_UnlockPage(u32 addr, u32 size = PAGE_SIZE) {}
inline static u32 bm_getRamOffset(void *p) {
	return 0;
}

#endif
