#include "types.h"
#include <unordered_set>

#include "hw/sh4/sh4_interpreter.h"
#include "hw/sh4/sh4_core.h"
#include "hw/sh4/sh4_interrupts.h"

#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/modules/mmu.h"

#include "blockmanager.h"
#include "ngen.h"
#include "decoder.h"
#include "oslib/virtmem.h"

#if FEAT_SHREC != DYNAREC_NONE

constexpr u32 CODE_SIZE = 10_MB;
constexpr u32 TEMP_CODE_SIZE = 1_MB;
constexpr u32 FULL_SIZE = CODE_SIZE + TEMP_CODE_SIZE;
DECLARE_CODE_CACHE(SH4_TCB, FULL_SIZE)

static u8* CodeCache;
static u8* TempCodeCache;
ptrdiff_t cc_rx_offset;

static std::unordered_set<u32> smc_hotspots;

static Sh4CodeBuffer codeBuffer;
Sh4Dynarec *sh4Dynarec;
Sh4Recompiler *Sh4Recompiler::Instance;

void *Sh4CodeBuffer::get()
{
	return tempBuffer ? &TempCodeCache[tempLastAddr] : &CodeCache[lastAddr];
}

void Sh4CodeBuffer::advance(u32 size)
{
	if (tempBuffer)
		tempLastAddr += size;
	else
		lastAddr += size;
}

u32 Sh4CodeBuffer::getFreeSpace()
{
	if (tempBuffer)
		return TEMP_CODE_SIZE - tempLastAddr;
	else
		return CODE_SIZE - lastAddr;
}

void *Sh4CodeBuffer::getBase()
{
	return CodeCache;
}

u32 Sh4CodeBuffer::getSize()
{
	return FULL_SIZE;
}

void rdv_codeRange(uintptr_t& start, uintptr_t& end)
{
	start = (uintptr_t)CC_RW2RX(CodeCache);
	end = start + FULL_SIZE;
}

void Sh4CodeBuffer::reset(bool temporary)
{
	if (temporary)
		tempLastAddr = 0;
	else
		lastAddr = 0;
}

void Sh4Recompiler::clear_temp_cache(bool full)
{
	//printf("recSh4:Temp Code Cache clear at %08X\n", curr_pc);
	codeBuffer.reset(true);
	bm_ResetTempCache(full);
}

extern u32 bmStatCacheResets;

void Sh4Recompiler::ResetCache()
{
	bmStatCacheResets++;
	INFO_LOG(DYNAREC, "recSh4:Dynarec Cache clear at %08X free space %d", getContext()->pc, codeBuffer.getFreeSpace());
	codeBuffer.reset(false);
	bm_ResetCache();
	smc_hotspots.clear();
	clear_temp_cache(true);
}

void Sh4Recompiler::Run()
{
	getContext()->restoreHostRoundingMode();

	u8 *sh4_dyna_rcb = (u8 *)getContext() + sizeof(Sh4Context);
	INFO_LOG(DYNAREC, "cntx // fpcb offset: %td // pc offset: %td // pc %08X", (u8*)p_sh4rcb->fpcb - sh4_dyna_rcb,
			(u8*)&getContext()->pc - sh4_dyna_rcb, getContext()->pc);
	
	sh4Dynarec->mainloop(sh4_dyna_rcb);

	getContext()->CpuRunning = false;
}

void AnalyseBlock(RuntimeBlockInfo* blk);

bool RuntimeBlockInfo::Setup(u32 rpc,fpscr_t rfpu_cfg)
{
	addr = host_code_size = 0;
	guest_cycles = guest_opcodes = host_opcodes = 0;
	sh4_code_size = 0;
	pBranchBlock=pNextBlock=0;
	code=0;
	has_jcond=false;
	BranchBlock = NullAddress;
	NextBlock = NullAddress;
	BlockType = BET_SCL_Intr;
	has_fpu_op = false;
	temp_block = false;
	
	vaddr = rpc;
	if (vaddr & 1)
	{
		// read address error
		Do_Exception(vaddr, Sh4Ex_AddressErrorRead);
		return false;
	}
	else if (mmu_enabled())
	{
		MmuError rv = mmu_instruction_translation(vaddr, addr);
		if (rv != MmuError::NONE)
		{
			DoMMUException(vaddr, rv, MMU_TT_IREAD);
			return false;
		}
	}
	else
	{
		addr = vaddr;
	}
	fpu_cfg=rfpu_cfg;
	
	oplist.clear();
	// Deadcast: as many as most blocks have, at once
	oplist.reserve(32);

	try {
		if (!dec_DecodeBlock(this, SH4_TIMESLICE / 2))
			return false;
	}
	catch (const SH4ThrownException& ex) {
		Do_Exception(rpc, ex.expEvn);
		return false;
	}
	SetProtectedFlags();

	AnalyseBlock(this);

	return true;
}

u64 bmStatCompileUs;	// Deadcast: time spent compiling, for the perf line (spg.cpp)

static u64 nowUs()
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (u64)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static DynarecCodeEntryPtr compilePCTimed(u32 blockcheck_failures);

//Called to compile code @pc
static DynarecCodeEntryPtr compilePC(u32 blockcheck_failures)
{
	const u64 start = nowUs();
	DynarecCodeEntryPtr code = compilePCTimed(blockcheck_failures);
	bmStatCompileUs += nowUs() - start;
	return code;
}

static DynarecCodeEntryPtr compilePCTimed(u32 blockcheck_failures)
{
	const u32 pc = Sh4cntx.pc;

	if (codeBuffer.getFreeSpace() < 32_KB || pc == 0x8c0000e0 || pc == 0xac010000 || pc == 0xac008300)
		Sh4Recompiler::Instance->ResetCache();

	// Deadcast: rewritten code: its version compiled before, if parked (bm_UnparkBlock)
	if (mmu_enabled() && !smc_hotspots.empty())
	{
		u32 paddr;
		if (mmu_instruction_translation(pc, paddr) == MmuError::NONE && smc_hotspots.count(paddr) != 0)
			if (DynarecCodeEntryPtr code = bm_UnparkBlock(paddr, pc, Sh4cntx.fpscr))
				return code;
	}

	RuntimeBlockInfo* rbi = sh4Dynarec->allocateBlock();

	if (!rbi->Setup(pc, Sh4cntx.fpscr))
	{
		delete rbi;
		return nullptr;
	}
	rbi->blockcheck_failures = blockcheck_failures;
	// Deadcast: under the MMU, as any block, its code kept (bm_SmcVariantCompiled), unless rewritten too often
	const bool smcVariant = mmu_enabled() && smc_hotspots.count(rbi->addr) != 0 && bm_SmcVariantsUsable(rbi->addr);
	if (!smcVariant && smc_hotspots.find(rbi->addr) != smc_hotspots.end())
	{
		codeBuffer.useTempBuffer(true);
		if (codeBuffer.getFreeSpace() < 32_KB)
			Sh4Recompiler::Instance->clear_temp_cache(false);
		rbi->temp_block = true;
		// Deadcast: never linked under the MMU, either way: the temp cache is cleared without undoing links
		// (bm_ResetTempCache), and a block linked to one jumped into whatever was compiled over it
		rbi->mmuNoLink = true;
		if (rbi->read_only)
			INFO_LOG(DYNAREC, "WARNING: temp block %x (%x) is protected!", rbi->vaddr, rbi->addr);
	}
	bool do_opts = !rbi->temp_block;
	bool block_check = !rbi->read_only;
	sh4Dynarec->compile(rbi, block_check, do_opts);
	verify(rbi->code != nullptr);
	// Deadcast: its ops are not needed once compiled (some 7 KB a block)
	std::vector<shil_opcode>().swap(rbi->oplist);
	if (smcVariant && block_check)
		bm_SmcVariantCompiled(rbi);

	bm_AddBlock(rbi);

	codeBuffer.useTempBuffer(false);

	return rbi->code;
}

bool rdv_Precompile(u32 vaddr, u32 fpscr, const std::function<bool(u32, u32)>& sameCode)
{
	// where decoding cannot fault (no exception may be raised here) and a block is not there already
	if ((vaddr & 1) != 0 || mmu_enabled() || (vaddr & 0x1C000000) != 0x0C000000 || bm_HasBlock(vaddr)
			|| smc_hotspots.count(vaddr) != 0 || codeBuffer.getFreeSpace() < 1_MB)
		return false;
	RuntimeBlockInfo* rbi = sh4Dynarec->allocateBlock();
	fpscr_t fpu;
	fpu.full = fpscr;
	if (!rbi->Setup(vaddr, fpu) || !sameCode(rbi->addr, rbi->sh4_code_size))
	{
		delete rbi;
		return false;
	}
	rbi->blockcheck_failures = 0;
	sh4Dynarec->compile(rbi, !rbi->read_only, true);
	verify(rbi->code != nullptr);
	std::vector<shil_opcode>().swap(rbi->oplist);
	bm_AddBlock(rbi);
	return true;
}

DynarecCodeEntryPtr DYNACALL rdv_FailedToFindBlock_pc()
{
	return rdv_FailedToFindBlock(Sh4cntx.pc);
}

DynarecCodeEntryPtr DYNACALL rdv_FailedToFindBlock(u32 pc)
{
	//DEBUG_LOG(DYNAREC, "rdv_FailedToFindBlock %08x", pc);
	Sh4cntx.pc=pc;
	DynarecCodeEntryPtr code = compilePC(0);
	if (code == NULL)
		code = bm_GetCodeByVAddr(Sh4cntx.pc);
	else
		code = (DynarecCodeEntryPtr)CC_RW2RX(code);
	return code;
}

static void ngen_FailedToFindBlock_internal() {
	rdv_FailedToFindBlock(Sh4cntx.pc);
}

void (*ngen_FailedToFindBlock)() = &ngen_FailedToFindBlock_internal;

// addr must be the physical address of the start of the block
DynarecCodeEntryPtr DYNACALL rdv_BlockCheckFail(u32 addr)
{
	DEBUG_LOG(DYNAREC, "rdv_BlockCheckFail @ %08x", addr);
	u32 blockcheck_failures = 0;
	if (mmu_enabled())
	{
		RuntimeBlockInfoPtr block = bm_GetBlock(addr);
		if (block)
		{
			blockcheck_failures = block->blockcheck_failures + 1;
			if (blockcheck_failures > 5)
			{
				bool inserted = smc_hotspots.insert(addr).second;
				if (inserted)
					DEBUG_LOG(DYNAREC, "rdv_BlockCheckFail SMC hotspot @ %08x fails %d", addr, blockcheck_failures);
			}
			// Deadcast: a version of rewritten code, kept (bm_ParkBlock)
			if (!block->smcCode.empty() && !block->read_only)
				bm_ParkBlock(block.get());
			else
				bm_DiscardBlock(block.get());
		}
	}
	else
	{
		Sh4cntx.pc = addr;
		Sh4Recompiler::Instance->ResetCache();
	}
	return (DynarecCodeEntryPtr)CC_RW2RX(compilePC(blockcheck_failures));
}

//Finds or compiles code @pc
static DynarecCodeEntryPtr findOrCompile()
{
	DynarecCodeEntryPtr rv = bm_GetCodeByVAddr(Sh4cntx.pc);  // Returns exec addr
	if (rv == ngen_FailedToFindBlock)
		// as the main loop does: under the MMU, compiling raises an exception when the pc cannot be
		// fetched, and the code is then the exception handler's
		rv = rdv_FailedToFindBlock(Sh4cntx.pc);
	
	return rv;
}

void* DYNACALL rdv_LinkBlock(u8* code,u32 dpc)
{
	// code is the RX addr to return after, however bm_GetBlock returns RW
	//DEBUG_LOG(DYNAREC, "rdv_LinkBlock %p pc %08x", code, dpc);
	RuntimeBlockInfoPtr rbi = bm_GetBlock(code);
	bool stale_block = false;
	if (!rbi)
	{
		stale_block = true;
		rbi = bm_GetStaleBlock(code);
	}
	
	verify(rbi != NULL);

	u32 bcls = BET_GET_CLS(rbi->BlockType);

	if (bcls == BET_CLS_Static)
	{
		if (rbi->BlockType == BET_StaticIntr)
			Sh4cntx.pc = rbi->NextBlock;
		else
			Sh4cntx.pc = rbi->BranchBlock;
	}
	else if (bcls == BET_CLS_Dynamic)
	{
		Sh4cntx.pc = dpc;
	}
	else if (bcls == BET_CLS_COND)
	{
		if (dpc)
			Sh4cntx.pc = rbi->BranchBlock;
		else
			Sh4cntx.pc = rbi->NextBlock;
	}

	DynarecCodeEntryPtr rv = findOrCompile();  // Returns rx ptr

	if (mmu_enabled())
	{
		// Deadcast: the exits mmu_canLink allowed are linked when the target is the block compiled for
		// that very address, in the same physical page for user space. If not (an exception was
		// raised looking it up, say), the block's exits are no longer linked.
		if (stale_block || bcls == BET_CLS_Dynamic || rbi->BlockType == BET_StaticIntr)
			return (void *)rv;
		u32 target = bcls == BET_CLS_COND && !dpc ? rbi->NextBlock : rbi->BranchBlock;
		RuntimeBlockInfoPtr nxt = bm_GetBlock((void *)rv);
		if (Sh4cntx.pc == target && nxt && nxt->vaddr == target && !nxt->temp_block
				&& ((target >> 31) != 0 || (nxt->addr >> 12) == (rbi->addr >> 12)))
		{
			if (bcls == BET_CLS_COND && !dpc)
				rbi->pNextBlock = nxt.get();
			else
				rbi->pBranchBlock = nxt.get();
			nxt->AddRef(rbi);
		}
		else
		{
			rbi->mmuNoLink = true;
		}
		u32 ncs = rbi->relink_offset + rbi->Relink();
		verify(rbi->host_code_size >= ncs);
		rbi->host_code_size = ncs;
		return (void *)rv;
	}
	if (!stale_block)
	{
		if (bcls == BET_CLS_Dynamic)
		{
			verify(rbi->relink_data == 0 || rbi->pBranchBlock == NULL);

			if (rbi->pBranchBlock != NULL)
			{
				rbi->pBranchBlock->RemRef(rbi);
				rbi->pBranchBlock = NULL;
				rbi->relink_data = 1;
			}
			else if (rbi->relink_data == 0)
			{
				rbi->pBranchBlock = bm_GetBlock(Sh4cntx.pc).get();
				rbi->pBranchBlock->AddRef(rbi);
			}
		}
		else
		{
			RuntimeBlockInfo* nxt = bm_GetBlock(Sh4cntx.pc).get();

			if (rbi->BranchBlock == Sh4cntx.pc)
				rbi->pBranchBlock = nxt;
			if (rbi->NextBlock == Sh4cntx.pc)
				rbi->pNextBlock = nxt;

			nxt->AddRef(rbi);
		}
		u32 ncs = rbi->relink_offset + rbi->Relink();
		verify(rbi->host_code_size >= ncs);
		rbi->host_code_size = ncs;
	}
	else
	{
		INFO_LOG(DYNAREC, "null RBI: from %08X to %08X -- unlinked stale block -- code %p next %p", rbi->vaddr, Sh4cntx.pc, code, rv);
	}
	
	return (void*)rv;
}

void Sh4Recompiler::Reset(bool hard)
{
	super::Reset(hard);
	if (hard)
		// in case ram base has changed
		sh4Dynarec->init(*getContext(), codeBuffer);
	ResetCache();
	if (hard)
		bm_Reset();
}

void Sh4Recompiler::Init()
{
	INFO_LOG(DYNAREC, "Sh4Recompiler::Init");
	super::Init();
	bm_Init();
	
	if (addrspace::virtmemEnabled())
		verify(&mem_b[0] == ((u8*)getContext()->sq_buffer + sizeof(Sh4Context) + 0x0C000000));

	// Call the platform-specific magic to make the pages RWX
	CodeCache = nullptr;
#ifdef FEAT_NO_RWX_PAGES
	bool rc = virtmem::prepare_jit_block(SH4_TCB, FULL_SIZE, (void**)&CodeCache, &cc_rx_offset);
#else
	bool rc = virtmem::prepare_jit_block(SH4_TCB, FULL_SIZE, (void**)&CodeCache);
#endif
	verify(rc);
	// Ensure the pointer returned is non-null
	verify(CodeCache != nullptr);

	TempCodeCache = CodeCache + CODE_SIZE;
	sh4Dynarec->init(*getContext(), codeBuffer);
	bm_ResetCache();
}

void Sh4Recompiler::Term()
{
	INFO_LOG(DYNAREC, "Sh4Recompiler::Term");
#ifdef FEAT_NO_RWX_PAGES
	if (CodeCache != nullptr)
		virtmem::release_jit_block(CodeCache, (u8 *)CodeCache + cc_rx_offset, FULL_SIZE);
#else
	if (CodeCache != nullptr && CodeCache != SH4_TCB)
		virtmem::release_jit_block(CodeCache, FULL_SIZE);
#endif
	CodeCache = nullptr;
	TempCodeCache = nullptr;
	bm_Term();
	super::Term();
}

Sh4Executor *Get_Sh4Recompiler()
{
	return new Sh4Recompiler();
}

static bool translateAddress(u32 addr, int size, u32 access, u32& outAddr, RuntimeBlockInfo* block)
{
	if (mmu_enabled() && mmu_is_translated(addr, size))
	{
		if (addr & (size - 1))
			// Unaligned
			return false;
		if (block != nullptr
				&& (addr >> 12) != (block->vaddr >> 12)
				&& (addr >> 12) != ((block->vaddr + block->sh4_code_size - 1) >> 12))
			// When full mmu is on, only consider addresses in the same 4k page
			return false;

		u32 paddr;
		MmuError rv = access == MMU_TT_DREAD ?
				mmu_data_translation<MMU_TT_DREAD>(addr, paddr)
				: mmu_data_translation<MMU_TT_DWRITE>(addr, paddr);
		if (rv != MmuError::NONE)
			return false;

		addr = paddr;
	}
	outAddr = addr;

	return true;
}

bool rdv_readMemImmediate(u32 addr, int size, void*& ptr, bool& isRam, u32& physAddr, RuntimeBlockInfo* block)
{
	size = std::min(size, 4);
	if (!translateAddress(addr, size, MMU_TT_DREAD, physAddr, block))
		return false;
	ptr = addrspace::readConst(physAddr, isRam, size);

	return true;
}

bool rdv_writeMemImmediate(u32 addr, int size, void*& ptr, bool& isRam, u32& physAddr, RuntimeBlockInfo* block)
{
	size = std::min(size, 4);
	if (!translateAddress(addr, size, MMU_TT_DWRITE, physAddr, block))
		return false;
	ptr = addrspace::writeConst(physAddr, isRam, size);

	return true;
}

void rdv_SetFailedToFindBlockHandler(void (*handler)())
{
	ngen_FailedToFindBlock = handler;
}
#endif  // FEAT_SHREC != DYNAREC_NONE
