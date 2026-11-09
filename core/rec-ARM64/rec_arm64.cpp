/*
	Copyright 2019 flyinghead

	This file is part of reicast.

    reicast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    reicast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with reicast.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "types.h"

#if FEAT_SHREC == DYNAREC_JIT && HOST_CPU == CPU_ARM64

#ifndef _M_ARM64
#include <unistd.h>
#endif
#include <list>
#include <map>
#include <unordered_set>

#include <aarch64/macro-assembler-aarch64.h>
using namespace vixl::aarch64;

//#define NO_BLOCK_LINKING

#include "hw/sh4/sh4_opcode_list.h"

#include "hw/sh4/sh4_mmr.h"
#include "hw/sh4/sh4_interrupts.h"
#include "arm64_unwind.h"
#include "hw/sh4/sh4_core.h"
#include "hw/sh4/dyna/ngen.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/sh4_rom.h"
#include "arm64_regalloc.h"
#include "hw/mem/addrspace.h"
#include "oslib/virtmem.h"
#include "emulator.h"
#include "deadcast/waitloops.h"
#include "deadcast/profiler.h"
#include "deadcast/mmuwindows.h"

struct DynaRBI : RuntimeBlockInfo
{
	DynaRBI(Sh4Context& sh4ctx, Sh4CodeBuffer& codeBuffer)
	: sh4ctx(sh4ctx), codeBuffer(codeBuffer) {}
	u32 Relink() override;

private:
	Sh4Context& sh4ctx;
	Sh4CodeBuffer& codeBuffer;
};

static u64 jmp_stack;
static Arm64UnwindInfo unwinder;

static void (*mainloop)(void *context);
static void (*handleException)();

struct DynaCode;

static DynaCode *arm64_intc_sched;
static DynaCode *arm64_no_update;
static DynaCode *blockCheckFail;
static DynaCode *checkBlockAddr;
static DynaCode *checkBlockFpu;
static DynaCode *linkBlockGenericStub;
static DynaCode *linkBlockBranchStub;
static DynaCode *linkBlockNextStub;
static DynaCode *writeStoreQueue32;
// Deadcast: R0-R15 and T pinned in host registers (arm64_regalloc.h), for the main loop as generated and
// the blocks compiled for it: without the MMU, with the dynarec's window (addrspace.h)
static bool pinnedRegs;
// Deadcast: how translated code calls C while registers are pinned: callThunk calls x16, the others one
// function each (rewrite() turns a faulting access into a call to one). The memory handlers' and
// lightThunk (the store queue flush) are light: they keep only the host registers C may change.
static DynaCode *callThunk;
static DynaCode *lightThunk;
// Deadcast: a store queue's flush (pref) while R0-R15 and T are pinned: the TA's polygon FIFO fed right
// there, the rest through the handler (GenMainloop)
static DynaCode *sqFlush;
// Deadcast: the SH4 addresses of stores that wrote to the store queues (rewrite): compiled inline, straight to sq_buffer
static std::unordered_set<u32> sqStoreSites;
// the TA's, for it (ta.cpp): its state machine (the state at [2048]), the transitions to handle, its
// data's write pointer and the data's start (that many bytes after it), its context, the data's size
extern u8 ta_fsm[2049];
void DYNACALL ta_vtx_handle(u32 trans);
extern u8 **const taDataWrite;
extern const ptrdiff_t taDataRootOffset;
extern void **const taContext;
extern const size_t taDataSize;
static std::vector<std::pair<uintptr_t, DynaCode *>> callThunks;

// Deadcast: MMU accesses the window could not do (the stubs, genMmuAccessStubs): translated, raising
// the exception if any, RAM mapped into the window for next time, the rest through the handlers.
// Store queue writes go to the store queues.
static bool isRam(u32 paddr) {
	return (paddr & 0x1C000000) == 0x0C000000;
}
static u32 handlerAddress(u32 vaddr, u32 paddr) {
	return (vaddr & 0x80000000) != 0 ? vaddr : paddr;
}
template<typename T>
static T mmuRead(u32 vaddr, u32 pc)
{
	const u32 paddr = mmuDynarecLookup(vaddr, 0, pc);
	if (isRam(paddr))
	{
		mmuwin::mapPage(vaddr, paddr);
		T value;
		memcpy(&value, addrspace::ram_base + 0x0C000000 + (paddr & RAM_MASK), sizeof(T));
		return value;
	}
	return addrspace::readt<T>(handlerAddress(vaddr, paddr));
}
static u32 DYNACALL mmuRead8(u32 vaddr, u32 pc) {
	return (int32_t)(int8_t)mmuRead<u8>(vaddr, pc);
}
static u32 DYNACALL mmuRead16(u32 vaddr, u32 pc) {
	return (int32_t)(int16_t)mmuRead<u16>(vaddr, pc);
}
static u32 DYNACALL mmuRead32(u32 vaddr, u32 pc) {
	return mmuRead<u32>(vaddr, pc);
}
static u64 DYNACALL mmuRead64(u32 vaddr, u32 pc) {
	return mmuRead<u64>(vaddr, pc);
}
template<typename T>
static void mmuWrite(u32 vaddr, u32 pc, T data)
{
	if (sizeof(T) >= 4 && (vaddr >> 26) == 0x38)
	{
		memcpy((u8 *)&p_sh4rcb->cntx + (vaddr & 0x3f), &data, sizeof(data));	// sq_buffer
		return;
	}
	const u32 paddr = mmuDynarecLookup(vaddr, 1, pc);
	if (isRam(paddr))
	{
		mmuwin::mapPage(vaddr, paddr);
		// a code page's protection faults, as any store to it (bm_RamWriteAccess)
		memcpy(addrspace::ram_base + 0x0C000000 + (paddr & RAM_MASK), &data, sizeof(T));
		return;
	}
	addrspace::writet<T>(handlerAddress(vaddr, paddr), data);
}
static void DYNACALL mmuWrite8(u32 vaddr, u32 pc, u32 data) {
	mmuWrite<u8>(vaddr, pc, data);
}
static void DYNACALL mmuWrite16(u32 vaddr, u32 pc, u32 data) {
	mmuWrite<u16>(vaddr, pc, data);
}
static void DYNACALL mmuWrite32(u32 vaddr, u32 pc, u32 data) {
	mmuWrite<u32>(vaddr, pc, data);
}
static void DYNACALL mmuWrite64(u32 vaddr, u32 pc, u64 data) {
	mmuWrite<u64>(vaddr, pc, data);
}
static DynaCode *writeStoreQueue64;

static void jitWriteProtect(Sh4CodeBuffer &codeBuffer, bool enable)
{
#ifdef TARGET_IPHONE
    if (enable)
    	virtmem::region_set_exec(codeBuffer.getBase(), codeBuffer.getSize());
    else
    	virtmem::region_unlock(codeBuffer.getBase(), codeBuffer.getSize());
#else
    JITWriteProtect(enable);
#endif
}

static void interpreter_fallback(Sh4Context *ctx, u16 op, OpCallFP *oph, u32 pc)
{
	try {
		oph(ctx, op);
	} catch (SH4ThrownException& ex) {
		if (pc & 1)
		{
			// Delay slot
			AdjustDelaySlotException(ex);
			pc--;
		}
		Do_Exception(pc, ex.expEvn);
		handleException();
	}
}

static void do_sqw_mmu_no_ex(u32 addr, Sh4Context *ctx, u32 pc)
{
	try {
		ctx->doSqWrite(addr, ctx);
	} catch (SH4ThrownException& ex) {
		if (pc & 1)
		{
			// Delay slot
			AdjustDelaySlotException(ex);
			pc--;
		}
		Do_Exception(pc, ex.expEvn);
		handleException();
	}
}

class Arm64Assembler : public MacroAssembler
{
	typedef void (MacroAssembler::*Arm64Op_RRO)(const Register&, const Register&, const Operand&);
	typedef void (MacroAssembler::*Arm64Op_RROF)(const Register&, const Register&, const Operand&, enum FlagsUpdate);
	typedef void (MacroAssembler::*Arm64Fop_RRR)(const VRegister&, const VRegister&, const VRegister&);

public:
	Arm64Assembler(Sh4Context& sh4ctx, Sh4CodeBuffer& codeBuffer)
	: Arm64Assembler(sh4ctx, codeBuffer, codeBuffer.get()) { }

	Arm64Assembler(Sh4Context& sh4ctx, Sh4CodeBuffer& codeBuffer, void *buffer)
	: MacroAssembler((u8 *)buffer, codeBuffer.getFreeSpace()), regalloc(this), sh4ctx(sh4ctx), codeBuffer(codeBuffer)
	{
		call_regs.push_back((const WRegister*)&w0);
		call_regs.push_back((const WRegister*)&w1);
		call_regs.push_back((const WRegister*)&w2);
		call_regs.push_back((const WRegister*)&w3);
		call_regs.push_back((const WRegister*)&w4);
		call_regs.push_back((const WRegister*)&w5);
		call_regs.push_back((const WRegister*)&w6);
		call_regs.push_back((const WRegister*)&w7);

		call_regs64.push_back((const XRegister*)&x0);
		call_regs64.push_back((const XRegister*)&x1);
		call_regs64.push_back((const XRegister*)&x2);
		call_regs64.push_back((const XRegister*)&x3);
		call_regs64.push_back((const XRegister*)&x4);
		call_regs64.push_back((const XRegister*)&x5);
		call_regs64.push_back((const XRegister*)&x6);
		call_regs64.push_back((const XRegister*)&x7);

		call_fregs.push_back(&s0);
		call_fregs.push_back(&s1);
		call_fregs.push_back(&s2);
		call_fregs.push_back(&s3);
		call_fregs.push_back(&s4);
		call_fregs.push_back(&s5);
		call_fregs.push_back(&s6);
		call_fregs.push_back(&s7);
	}

	void binaryOp_RRO(shil_opcode* op, Arm64Op_RRO arm_op, Arm64Op_RROF arm_op2)
	{
		Operand op3 = Operand(0);
		if (op->rs2.is_imm())
		{
			op3 = Operand(op->rs2._imm);
		}
		else if (op->rs2.is_r32i())
		{
			op3 = Operand(regalloc.MapRegister(op->rs2));
		}
		if (arm_op != NULL)
			((*this).*arm_op)(regalloc.MapRegister(op->rd), regalloc.MapRegister(op->rs1), op3);
		else
			((*this).*arm_op2)(regalloc.MapRegister(op->rd), regalloc.MapRegister(op->rs1), op3, LeaveFlags);
	}

	void binaryFop(shil_opcode* op, Arm64Fop_RRR arm_op)
	{
		VRegister reg1;
		VRegister reg2;
		if (op->rs1.is_imm())
		{
			Fmov(s0, reinterpret_cast<f32&>(op->rs1._imm));
			reg1 = s0;
		}
		else
		{
			reg1 = regalloc.MapVRegister(op->rs1);
		}
		if (op->rs2.is_imm())
		{
			Fmov(s1, reinterpret_cast<f32&>(op->rs2._imm));
			reg2 = s1;
		}
		else
		{
			reg2 = regalloc.MapVRegister(op->rs2);
		}
		((*this).*arm_op)(regalloc.MapVRegister(op->rd), reg1, reg2);
	}

	const Register& GenMemAddr(const shil_opcode& op, const Register* raddr = NULL)
	{
		const Register* ret_reg = raddr == NULL ? &w0 : raddr;

		if (op.rs3.is_imm())
		{
			if (regalloc.IsAllocg(op.rs1))
				Add(*ret_reg, regalloc.MapRegister(op.rs1), op.rs3._imm);
			else
			{
				Ldr(*ret_reg, sh4_context_mem_operand(op.rs1._reg));
				Add(*ret_reg, *ret_reg, op.rs3._imm);
			}
		}
		else if (op.rs3.is_r32i())
		{
			if (regalloc.IsAllocg(op.rs1) && regalloc.IsAllocg(op.rs3))
				Add(*ret_reg, regalloc.MapRegister(op.rs1), regalloc.MapRegister(op.rs3));
			else
			{
				Ldr(*ret_reg, sh4_context_mem_operand(op.rs1._reg));
				Ldr(w3, sh4_context_mem_operand(op.rs3._reg));
				Add(*ret_reg, *ret_reg, w3);
			}
		}
		else if (!op.rs3.is_null())
		{
			die("invalid rs3");
		}
		else if (op.rs1.is_reg())
		{
			if (regalloc.IsAllocg(op.rs1))
			{
				if (raddr == NULL)
					ret_reg = &regalloc.MapRegister(op.rs1);
				else
					Mov(*ret_reg, regalloc.MapRegister(op.rs1));
			}
			else
			{
				Ldr(*ret_reg, sh4_context_mem_operand(op.rs1._reg));
			}
		}
		else
		{
			verify(op.rs1.is_imm());
			Mov(*ret_reg, op.rs1._imm);
		}

		return *ret_reg;
	}

	void compileBlock(RuntimeBlockInfo* block, bool force_checks, bool optimise)
	{
		//printf("REC-ARM64 compiling %08x\n", block->addr);
		jitWriteProtect(codeBuffer, false);
		this->block = block;
		block->mmuStubs.clear();
		block->mmuSites.clear();
		block->exitTestsT = false;
		CheckBlock(force_checks, block);
		
		// run register allocator
		regalloc.DoAlloc(block, pinnedRegs);

		// scheduler
		Ldr(w1, sh4_context_mem_operand(&sh4ctx.cycle_counter));
		Cmp(w1, 0);
		Label cycles_remaining;
		B(&cycles_remaining, pl);
		Mov(w0, block->vaddr);
		GenCall(arm64_intc_sched);
		Mov(w1, w0);
		Bind(&cycles_remaining);

		Sub(w1, w1, block->guest_cycles);
		Str(w1, sh4_context_mem_operand(&sh4ctx.cycle_counter));
		if (profilerCountsRuns())
		{
			// Deadcast: the profiler's count of the block's runs
			Mov(x2, reinterpret_cast<uintptr_t>(&block->runs));
			Ldr(x3, MemOperand(x2));
			Add(x3, x3, 1);
			Str(x3, MemOperand(x2));
		}
		if (deadcast::WaitLoopHook hook = deadcast::waitLoopHook(block))
		{
			// Deadcast: a known wait loop goes by without running while nothing changes (waitloops.cpp)
			Mov(w0, block->vaddr);
			GenCallRuntime(hook);
		}
		// Deadcast: the registers the block starts with, loaded at once
		regalloc.PreloadLiveIns(16, pinnedRegs ? 2 : 6, 6);

		for (size_t i = 0; i < block->oplist.size(); i++)
		{
			shil_opcode& op  = block->oplist[i];
			regalloc.OpBegin(&op, i);

			switch (op.op)
			{
			case shop_ifb:	// Interpreter fallback
				if (op.rs1._imm)	// if NeedPC()
				{
					Mov(w0, op.rs2._imm);
					Str(w0, sh4_context_mem_operand(&sh4ctx.pc));
				}

				Mov(x0, x28);
				Mov(w1, op.rs3._imm);
				if (!mmu_enabled())
				{
					GenCallRuntime(OpDesc[op.rs3._imm]->oph);
				}
				else
				{
					Mov(x2, reinterpret_cast<uintptr_t>(*OpDesc[op.rs3._imm]->oph));	// op handler
					Mov(w3, block->vaddr + op.guest_offs - (op.delay_slot ? 1 : 0));	// pc

					GenCallRuntime(interpreter_fallback);
					// Deadcast: it may have written PTEH, changing the ASID and its window
					genReloadWindow();
				}

				break;

			case shop_jcond:
			case shop_jdyn:
				{
					const Register rd = regalloc.MapRegister(op.rd);
					if (op.rs2.is_imm())
						Add(rd, regalloc.MapRegister(op.rs1), op.rs2._imm);
					else
						Mov(rd, regalloc.MapRegister(op.rs1));
					// Save it for the branching at the end of the block
					Mov(w29, rd);
				}
				break;

			case shop_mov32:
				verify(op.rd.is_reg());
				verify(op.rs1.is_reg() || op.rs1.is_imm());

				if (regalloc.IsAllocf(op.rd))
				{
					const VRegister rd = regalloc.MapVRegister(op.rd);
					if (op.rs1.is_imm())
						Fmov(rd, reinterpret_cast<f32&>(op.rs1._imm));
					else if (regalloc.IsAllocf(op.rs1))
						Fmov(rd, regalloc.MapVRegister(op.rs1));
					else
						Fmov(rd, regalloc.MapRegister(op.rs1));
				}
				else
				{
					const Register rd = regalloc.MapRegister(op.rd);
					if (op.rs1.is_imm())
						Mov(rd, op.rs1._imm);
					else if (regalloc.IsAllocg(op.rs1))
						Mov(rd, regalloc.MapRegister(op.rs1));
					else
						Fmov(rd, regalloc.MapVRegister(op.rs1));
				}
				break;

			case shop_mov64:
				{
					verify(op.rd.is_reg());
					verify(op.rs1.is_reg() || op.rs1.is_imm());

					if (!regalloc.IsAllocf(op.rd))
					{
						verify(!regalloc.IsAllocf(op.rs1));
						shil_param_to_host_reg(op.rs1, x3);
						host_reg_to_shil_param(op.rd, x3);
					}
					else
					{
						const VRegister& rd0 = regalloc.MapVRegister(op.rd, 0);
						const VRegister& rs0 = regalloc.MapVRegister(op.rs1, 0);
						const VRegister& rd1 = regalloc.MapVRegister(op.rd, 1);
						const VRegister& rs1 = regalloc.MapVRegister(op.rs1, 1);
						if (rd0.Is(rs1))
						{
							Fmov(w0, rd0);
							Fmov(rd0, rs0);
							Fmov(rd1, w0);
						}
						else
						{
							if (!rd0.Is(rs0))
								Fmov(rd0, rs0);
							if (!rd1.Is(rs1))
								Fmov(rd1, rs1);
						}
					}
				}
				break;

			case shop_readm:
				GenReadMemory(op, i, optimise);
				break;

			case shop_writem:
				GenWriteMemory(op, i, optimise);
				break;

			case shop_sync_sr:
				GenCallRuntime(UpdateSR);
				break;
			case shop_sync_fpscr:
				Mov(x0, x28);
				GenCallRuntime(Sh4Context::UpdateFPSCR);
				break;

			case shop_swaplb:
				{
					const Register rs1 = regalloc.MapRegister(op.rs1);
					const Register rd = regalloc.MapRegister(op.rd);
					Mov(w0, Operand(rs1, LSR, 16));
					Rev16(rd, rs1);
					Bfi(rd, w0, 16, 16);
				}
				break;

			case shop_neg:
				Neg(regalloc.MapRegister(op.rd), regalloc.MapRegister(op.rs1));
				break;
			case shop_not:
				Mvn(regalloc.MapRegister(op.rd), regalloc.MapRegister(op.rs1));
				break;

			case shop_and:
				binaryOp_RRO(&op, &MacroAssembler::And, NULL);
				break;
			case shop_or:
				binaryOp_RRO(&op, &MacroAssembler::Orr, NULL);
				break;
			case shop_xor:
				binaryOp_RRO(&op, &MacroAssembler::Eor, NULL);
				break;
			case shop_add:
				binaryOp_RRO(&op, NULL, &MacroAssembler::Add);
				break;
			case shop_sub:
				binaryOp_RRO(&op, NULL, &MacroAssembler::Sub);
				break;
			case shop_shl:
				if (op.rs2.is_imm())
					Lsl(regalloc.MapRegister(op.rd), regalloc.MapRegister(op.rs1), op.rs2._imm);
				else if (op.rs2.is_reg())
					Lsl(regalloc.MapRegister(op.rd), regalloc.MapRegister(op.rs1), regalloc.MapRegister(op.rs2));
				break;
			case shop_shr:
				if (op.rs2.is_imm())
					Lsr(regalloc.MapRegister(op.rd), regalloc.MapRegister(op.rs1), op.rs2._imm);
				else if (op.rs2.is_reg())
					Lsr(regalloc.MapRegister(op.rd), regalloc.MapRegister(op.rs1), regalloc.MapRegister(op.rs2));
				break;
			case shop_sar:
				if (op.rs2.is_imm())
					Asr(regalloc.MapRegister(op.rd), regalloc.MapRegister(op.rs1), op.rs2._imm);
				else if (op.rs2.is_reg())
					Asr(regalloc.MapRegister(op.rd), regalloc.MapRegister(op.rs1), regalloc.MapRegister(op.rs2));
				break;
			case shop_ror:
				if (op.rs2.is_imm())
					Ror(regalloc.MapRegister(op.rd), regalloc.MapRegister(op.rs1), op.rs2._imm);
				else if (op.rs2.is_reg())
					Ror(regalloc.MapRegister(op.rd), regalloc.MapRegister(op.rs1), regalloc.MapRegister(op.rs2));
				break;

			case shop_adc:
				{
					Register reg1;
					Operand op2 = Operand(0);
					Register reg3;
					if (op.rs1.is_imm())
					{
						Mov(w0, op.rs1.imm_value());
						reg1 = w0;
					}
					else
					{
						reg1 = regalloc.MapRegister(op.rs1);
					}
					if (op.rs2.is_imm())
						op2 = Operand(op.rs2.imm_value());
					else
						op2 = regalloc.MapRegister(op.rs2);
					if (op.rs3.is_imm())
					{
						Mov(w1, op.rs3.imm_value());
						reg3 = w1;
					}
					else
					{
						reg3 = regalloc.MapRegister(op.rs3);
					}
					Cmp(reg3, 1);	// C = rs3
					Adcs(regalloc.MapRegister(op.rd), reg1, op2); // (C,rd)=rs1+rs2+rs3(C)
					Cset(regalloc.MapRegister(op.rd2), cs);	// rd2 = C
				}
				break;
			case shop_sbc:
				{
					Register reg1;
					Operand op2 = Operand(0);
					Operand op3 = Operand(0);
					if (op.rs1.is_imm())
					{
						Mov(w0, op.rs1.imm_value());
						reg1 = w0;
					}
					else
					{
						reg1 = regalloc.MapRegister(op.rs1);
					}
					if (op.rs2.is_imm())
						op2 = Operand(op.rs2.imm_value());
					else
						op2 = regalloc.MapRegister(op.rs2);
					if (op.rs3.is_imm())
						op3 = Operand(op.rs3.imm_value());
					else
						op3 = regalloc.MapRegister(op.rs3);
					Cmp(wzr, op3);	// C = ~rs3
					Sbcs(regalloc.MapRegister(op.rd), reg1, op2); // (C,rd) = rs1 - rs2 - ~rs3(C)
					Cset(regalloc.MapRegister(op.rd2), cc);	// rd2 = ~C
				}
				break;
			case shop_negc:
				{
					Operand op1 = Operand(0);
					Operand op2 = Operand(0);
					if (op.rs1.is_imm())
						op1 = Operand(op.rs1.imm_value());
					else
						op1 = regalloc.MapRegister(op.rs1);
					if (op.rs2.is_imm())
						op2 = Operand(op.rs2.imm_value());
					else
						op2 = regalloc.MapRegister(op.rs2);
					Cmp(wzr, op2);	// C = ~rs2
					Sbcs(regalloc.MapRegister(op.rd), wzr, op1);	// (C,rd) = 0 - rs1 - ~rs2(C)
					Cset(regalloc.MapRegister(op.rd2), cc);			// rd2 = ~C
				}
				break;

			case shop_rocr:
				{
					Register reg1;
					Register reg2;
					if (op.rs1.is_imm())
					{
						Mov(w1, op.rs1.imm_value());
						reg1 = w1;
					}
					else
					{
						reg1 = regalloc.MapRegister(op.rs1);
					}
					if (op.rs2.is_imm())
					{
						Mov(w2, op.rs2.imm_value());
						reg2 = w2;
					}
					else
					{
						reg2 = regalloc.MapRegister(op.rs2);
					}
					Ubfx(w0, reg1, 0, 1);										// w0 = rs1[0] (new C)
					const Register rd = regalloc.MapRegister(op.rd);
					Mov(rd, Operand(reg1, LSR, 1));	// rd = rs1 >> 1
					Bfi(rd, reg2, 31, 1);				// rd |= C << 31
					Mov(regalloc.MapRegister(op.rd2), w0);						// rd2 = w0 (new C)
				}
				break;
			case shop_rocl:
				{
					Register reg1;
					Register reg2;
					if (op.rs1.is_imm())
					{
						Mov(w0, op.rs1.imm_value());
						reg1 = w0;
					}
					else
					{
						reg1 = regalloc.MapRegister(op.rs1);
					}
					if (op.rs2.is_imm())
					{
						Mov(w1, op.rs2.imm_value());
						reg2 = w1;
					}
					else
					{
						reg2 = regalloc.MapRegister(op.rs2);
					}
					Tst(reg1, 0x80000000);						// Z = ~rs1[31]
					Orr(regalloc.MapRegister(op.rd), reg2, Operand(reg1, LSL, 1)); // rd = rs1 << 1 | rs2(C)
					Cset(regalloc.MapRegister(op.rd2), ne);		// rd2 = ~Z(C)
				}
				break;

			case shop_shld:
			case shop_shad:
				{
					Register reg1;
					if (op.rs1.is_imm())
					{
						Mov(w0, op.rs1.imm_value());
						reg1 = w0;
					}
					else
					{
						reg1 = regalloc.MapRegister(op.rs1);
					}
					Label positive_shift, negative_shift, end;
					const Register rs2 = regalloc.MapRegister(op.rs2);
					Tbz(rs2, 31, &positive_shift);
					Cmn(rs2, 32);
					B(&negative_shift, ne);
					const Register rd = regalloc.MapRegister(op.rd);
					// rs2 == -32 => rd = 0 (logical) or 0/-1 (arith)
					if (op.op == shop_shld)
						// Logical shift
						//Lsr(rd, reg1, 31);
						Mov(rd, wzr);
					else
						// Arithmetic shift
						Asr(rd, reg1, 31);
					B(&end);

					Bind(&positive_shift);
					// rs2 >= 0 => left shift
					Lsl(rd, reg1, rs2);
					B(&end);

					Bind(&negative_shift);
					// rs2 < 0 => right shift
					Neg(w1, rs2);
					if (op.op == shop_shld)
						// Logical shift
						Lsr(rd, reg1, w1);
					else
						// Arithmetic shift
						Asr(rd, reg1, w1);
					Bind(&end);
				}
				break;

			case shop_test:
			case shop_seteq:
			case shop_setge:
			case shop_setgt:
			case shop_setae:
			case shop_setab:
				{
					const Register rs1 = regalloc.MapRegister(op.rs1);
					if (op.op == shop_test)
					{
						if (op.rs2.is_imm())
							Tst(rs1, op.rs2._imm);
						else
							Tst(rs1, regalloc.MapRegister(op.rs2));
					}
					else
					{
						if (op.rs2.is_imm())
							Cmp(rs1, op.rs2._imm);
						else
							Cmp(rs1, regalloc.MapRegister(op.rs2));
					}

					static const Condition shop_conditions[] = { eq, eq, ge, gt, hs, hi };

					Cset(regalloc.MapRegister(op.rd), shop_conditions[op.op - shop_test]);
				}
				break;
			case shop_setpeq:
				{
					Register reg1;
					Register reg2;
					if (op.rs1.is_imm())
					{
						Mov(w0, op.rs1.imm_value());
						reg1 = w0;
					}
					else
					{
						reg1 = regalloc.MapRegister(op.rs1);
					}
					if (op.rs2.is_imm())
					{
						Mov(w1, op.rs2.imm_value());
						reg2 = w1;
					}
					else
					{
						reg2 = regalloc.MapRegister(op.rs2);
					}
					Eor(w1, reg1, reg2);
					const Register rd = regalloc.MapRegister(op.rd);
					Mov(rd, wzr);
					Mov(w2, wzr);	// wzr not supported by csinc (?!)
					Tst(w1, 0xFF000000);
					Csinc(rd, rd, w2, ne);
					Tst(w1, 0x00FF0000);
					Csinc(rd, rd, w2, ne);
					Tst(w1, 0x0000FF00);
					Csinc(rd, rd, w2, ne);
					Tst(w1, 0x000000FF);
					Csinc(rd, rd, w2, ne);
				}
				break;

			case shop_mul_u16:
				{
					Register reg2;
					if (op.rs2.is_imm())
					{
						Mov(w0, op.rs2.imm_value());
						reg2 = w0;
					}
					else
					{
						reg2 = regalloc.MapRegister(op.rs2);
					}
					Uxth(w1, regalloc.MapRegister(op.rs1));
					Uxth(w2, reg2);
					Mul(regalloc.MapRegister(op.rd), w1, w2);
				}
				break;
			case shop_mul_s16:
				{
					Register reg2;
					if (op.rs2.is_imm())
					{
						Mov(w0, op.rs2.imm_value());
						reg2 = w0;
					}
					else
					{
						reg2 = regalloc.MapRegister(op.rs2);
					}
					Sxth(w1, regalloc.MapRegister(op.rs1));
					Sxth(w2, reg2);
					Mul(regalloc.MapRegister(op.rd), w1, w2);
				}
				break;
			case shop_mul_i32:
				{
					Register reg2;
					if (op.rs2.is_imm())
					{
						Mov(w0, op.rs2.imm_value());
						reg2 = w0;
					}
					else
					{
						reg2 = regalloc.MapRegister(op.rs2);
					}
					Mul(regalloc.MapRegister(op.rd), regalloc.MapRegister(op.rs1), reg2);
				}
				break;
			case shop_mul_u64:
			case shop_mul_s64:
				{
					Register reg2;
					if (op.rs2.is_imm())
					{
						Mov(w0, op.rs2.imm_value());
						reg2 = w0;
					}
					else
					{
						reg2 = regalloc.MapRegister(op.rs2);
					}
					const Register& rd_xreg = regalloc.MapRegister(op.rd).X();
					if (op.op == shop_mul_u64)
						Umull(rd_xreg, regalloc.MapRegister(op.rs1), reg2);
					else
						Smull(rd_xreg, regalloc.MapRegister(op.rs1), reg2);
					const Register& rd2_xreg = regalloc.MapRegister(op.rd2).X();
					Lsr(rd2_xreg, rd_xreg, 32);
				}
				break;

			case shop_pref:
				{
					Label not_sqw;
					if (op.rs1.is_imm())
						Mov(w0, op.rs1._imm);
					else
					{
						if (regalloc.IsAllocg(op.rs1))
							Lsr(w1, regalloc.MapRegister(op.rs1), 26);
						else
						{
							Ldr(w0, sh4_context_mem_operand(op.rs1._reg));
							Lsr(w1, w0, 26);
						}
						Cmp(w1, 0x38);
						B(&not_sqw, ne);
						if (regalloc.IsAllocg(op.rs1))
							Mov(w0, regalloc.MapRegister(op.rs1));
					}

					Mov(x1, x28);
					if (mmu_enabled())
					{
						// Deadcast: written unless translating the address raises an exception, which is
						// then raised out of line, the context completed first (genPrefSlowPaths)
						prefSlowPaths.emplace_back();
						PrefSlowPath& slow = prefSlowPaths.back();
						slow.addr = op.rs1;
						slow.pc = block->vaddr + op.guest_offs - (op.delay_slot ? 1 : 0);
						slow.dirty = regalloc.mmuDirty;
						GenCallRuntime(sqWriteMmuTry);
						Cbnz(w0, &slow.entry);
						Bind(&slow.back);
					}
					else if (pinnedRegs)
						GenCall(sqFlush);
					else
					{
						Ldr(x3, sh4_context_mem_operand(&sh4ctx.doSqWrite));
						Blr(x3);
					}
					Bind(&not_sqw);
				}
				break;

			case shop_ext_s8:
				Sxtb(regalloc.MapRegister(op.rd), regalloc.MapRegister(op.rs1));
				break;
			case shop_ext_s16:
				Sxth(regalloc.MapRegister(op.rd), regalloc.MapRegister(op.rs1));
				break;

			case shop_xtrct:
				{
					const Register rd = regalloc.MapRegister(op.rd);
					Register rs1;
					if (op.rs1.is_imm()) {
						rs1 = w1;
						Mov(rs1, op.rs1._imm);
					}
					else
					{
						rs1 = regalloc.MapRegister(op.rs1);
					}
					Register rs2;
					if (op.rs2.is_imm()) {
						rs2 = w2;
						Mov(rs2, op.rs2._imm);
					}
					else
					{
						rs2 = regalloc.MapRegister(op.rs2);
					}
					if (rd.Is(rs1))
					{
						verify(!rd.Is(rs2));
						Lsr(rd, rs1, 16);
						Lsl(w0, rs2, 16);
					}
					else
					{
						Lsl(rd, rs2, 16);
						Lsr(w0, rs1, 16);
					}
					Orr(rd, rd, w0);
				}
				break;

			//
			// FPU
			//

			case shop_fadd:
				binaryFop(&op, &MacroAssembler::Fadd);
				break;
			case shop_fsub:
				binaryFop(&op, &MacroAssembler::Fsub);
				break;
			case shop_fmul:
				binaryFop(&op, &MacroAssembler::Fmul);
				break;
			case shop_fdiv:
				binaryFop(&op, &MacroAssembler::Fdiv);
				break;

			case shop_fabs:
				Fabs(regalloc.MapVRegister(op.rd), regalloc.MapVRegister(op.rs1));
				break;
			case shop_fneg:
				Fneg(regalloc.MapVRegister(op.rd), regalloc.MapVRegister(op.rs1));
				break;
			case shop_fsqrt:
				Fsqrt(regalloc.MapVRegister(op.rd), regalloc.MapVRegister(op.rs1));
				break;

			case shop_fmac:
				Fmadd(regalloc.MapVRegister(op.rd), regalloc.MapVRegister(op.rs3), regalloc.MapVRegister(op.rs2), regalloc.MapVRegister(op.rs1));
				break;

			case shop_fsrra:
				Fsqrt(s0, regalloc.MapVRegister(op.rs1));
				Fmov(s1, 1.f);
				Fdiv(regalloc.MapVRegister(op.rd), s1, s0);
				break;

			case shop_fsetgt:
			case shop_fseteq:
				Fcmp(regalloc.MapVRegister(op.rs1), regalloc.MapVRegister(op.rs2));
				Cset(regalloc.MapRegister(op.rd), op.op == shop_fsetgt ? gt : eq);
				break;

			case shop_fsca:
				Mov(x1, reinterpret_cast<uintptr_t>(&sin_table));
				if (op.rs1.is_reg())
					Add(x1, x1, Operand(regalloc.MapRegister(op.rs1), UXTH, 3));
				else
					Add(x1, x1, Operand(op.rs1.imm_value() << 3));
				if (regalloc.IsAllocf(op.rd))
				{
					Ldp(regalloc.MapVRegister(op.rd, 0), regalloc.MapVRegister(op.rd, 1), MemOperand(x1));
				}
				else
				{
					Ldr(x2, MemOperand(x1));
					Str(x2, sh4_context_mem_operand(op.rd._reg));
				}
				break;

			// Deadcast: fipr and ftrv in single precision, fused multiply-adds: the SH4 computes them with
			// errors of its own, not exactly either (the interpreter's, in double, is exact)
			case shop_fipr:
				Add(x2, x28, op.rs1.reg_offset());
				Ld1(v0.V4S(), MemOperand(x2));
				Add(x2, x28, op.rs2.reg_offset());
				Ld1(v1.V4S(), MemOperand(x2));
				Fmul(v20.V4S(), v0.V4S(), v1.V4S());
				Faddp(v21.V4S(), v20.V4S(), v20.V4S());
				Faddp(s22, v21.V2S());
				host_reg_to_shil_param(op.rd, s22);
				break;

			case shop_ftrv:
				// fd[i] = sum over j of fn[j] * fm[j * 4 + i]: the lanes are the four results
				Add(x2, x28, op.rs1.reg_offset());
				Ld1(v0.V4S(), MemOperand(x2));
				Add(x2, x28, op.rs2.reg_offset());
				Ld1(v1.V4S(), v2.V4S(), v3.V4S(), v4.V4S(), MemOperand(x2));
				Fmul(v22.V4S(), v1.V4S(), v0.S(), 0);
				Fmla(v22.V4S(), v2.V4S(), v0.S(), 1);
				Fmla(v22.V4S(), v3.V4S(), v0.S(), 2);
				Fmla(v22.V4S(), v4.V4S(), v0.S(), 3);
				Add(x2, x28, op.rd.reg_offset());
				St1(v22.V4S(), MemOperand(x2));
				break;

			case shop_frswap:
				Add(x2, x28, op.rs1.reg_offset());
				Add(x3, x28, op.rd.reg_offset());
				Ld4(v0.V2D(), v1.V2D(), v2.V2D(), v3.V2D(), MemOperand(x2));
				Ld4(v4.V2D(), v5.V2D(), v6.V2D(), v7.V2D(), MemOperand(x3));
				St4(v4.V2D(), v5.V2D(), v6.V2D(), v7.V2D(), MemOperand(x2));
				St4(v0.V2D(), v1.V2D(), v2.V2D(), v3.V2D(), MemOperand(x3));
				break;

			case shop_cvt_f2i_t:
				{
					const VRegister& from = regalloc.MapVRegister(op.rs1);
					const Register& to = regalloc.MapRegister(op.rd);
					Fcvtzs(to, from);
					Mov(w0, 0x7FFFFF80);
					Cmp(to, w0);
					Mov(w0, 0x7FFFFFFF);
					Csel(to, w0, to, gt);
					Fcmp(from, from);
					Mov(w0, 0x80000000);
					Csel(to, to, w0, vc);
				}
				break;

			case shop_cvt_i2f_n:
			case shop_cvt_i2f_z:
				Scvtf(regalloc.MapVRegister(op.rd), regalloc.MapRegister(op.rs1));
				break;

			default:
				shil_chf[op.op](&op);
				break;
			}
			regalloc.OpEnd(&op);
		}
		if (BET_GET_CLS(block->BlockType) == BET_CLS_COND && !block->has_jcond)
		{
			// Deadcast: the condition of the exit in w29, as jcond leaves it (RelinkBlock): T, from its
			// register if it is in one. Pinned in w18, the exit tests it there instead (one cbz/cbnz in place of
			// a mov, a cmp and a b.ne at the end of every conditional block)
			const shil_param t(reg_sr_T);
			block->exitTestsT = pinnedRegs && regalloc.IsAllocg(t) && regalloc.MapRegister(t).Is(w18);
			if (block->exitTestsT)
				;
			else if (regalloc.IsAllocg(t))
				Mov(w29, regalloc.MapRegister(t));
			else
				Ldr(w29, sh4_context_mem_operand(&sh4ctx.sr.T));
		}
		regalloc.Cleanup();
		genMmuAccessStubs();
		genPrefSlowPaths();
		genBlockChecksFailed();

		block->relink_offset = (u32)GetBuffer()->GetCursorOffset();
		block->relink_data = 0;

		RelinkBlock(block);

		Finalize();
		jitWriteProtect(codeBuffer, true);
	}

	void canonStart(const shil_opcode *op)
	{
		CC_pars.clear();
	}

	void canonParam(const shil_opcode& op, const shil_param *prm, CanonicalParamType tp)
	{
		switch (tp)
		{

		case CPT_u32:
		case CPT_ptr:
		case CPT_f32:
		case CPT_sh4ctx:
		{
			CC_PS t = { tp, prm };
			CC_pars.push_back(t);
		}
		break;

		case CPT_u64rvL:
		case CPT_u32rv:
			host_reg_to_shil_param(*prm, w0);
			break;

		case CPT_u64rvH:
			Lsr(x1, x0, 32);
			host_reg_to_shil_param(*prm, w1);
			break;

		case CPT_f32rv:
			host_reg_to_shil_param(*prm, s0);
			break;
		}
	}

	void canonCall(const shil_opcode *op, void *function)
	{
		int regused = 0;
		int fregused = 0;

		// Args are pushed in reverse order by shil_canonical
		for (int i = CC_pars.size(); i-- > 0;)
		{
			verify(fregused < (int)call_fregs.size() && regused < (int)call_regs.size());
			const shil_param& prm = *CC_pars[i].prm;
			switch (CC_pars[i].type)
			{
			// push the params
			case CPT_u32:
				shil_param_to_host_reg(prm, *call_regs[regused++]);
				break;

			case CPT_f32:
				if (prm.is_reg())
					Fmov(*call_fregs[fregused], regalloc.MapVRegister(prm));
				else if (prm.is_imm())
					Fmov(*call_fregs[fregused], reinterpret_cast<const f32&>(prm._imm));
				else
					verify(prm.is_null());
				fregused++;
				break;

			case CPT_ptr:
				verify(prm.is_reg());
				// push the ptr itself
				Mov(*call_regs64[regused++], reinterpret_cast<uintptr_t>(prm.reg_ptr(sh4ctx)));
				break;

			case CPT_sh4ctx:
				Mov(*call_regs64[regused++], reinterpret_cast<uintptr_t>(&sh4ctx));
				break;

			case CPT_u32rv:
			case CPT_u64rvL:
			case CPT_u64rvH:
			case CPT_f32rv:
				// return values are handled in canonParam()
				break;
			}
		}
		GenCallRuntime((void (*)())function);
		for (const CC_PS& ccParam : CC_pars)
		{
			const shil_param& prm = *ccParam.prm;
			if (ccParam.type == CPT_ptr && prm.count() == 2 && regalloc.IsAllocf(prm) && (op->rd._reg == prm._reg || op->rd2._reg == prm._reg))
			{
				// fsca rd param is a pointer to a 64-bit reg so reload the regs if allocated
				Ldr(regalloc.MapVRegister(prm, 0), sh4_context_mem_operand(prm._reg));
				Ldr(regalloc.MapVRegister(prm, 1), sh4_context_mem_operand((Sh4RegType)(prm._reg + 1)));
			}
		}
	}

	MemOperand sh4_context_mem_operand(void *p)
	{
		u32 offset = (u8*)p - (u8*)&sh4ctx;
		verify((offset & 3) == 0 && offset <= 16380);	// FIXME 64-bit regs need multiple of 8 up to 32760
		return MemOperand(x28, offset);
	}
	MemOperand sh4_context_mem_operand(Sh4RegType reg)
	{
		u32 offset = getRegOffset(reg);
		verify((offset & 3) == 0 && offset <= 16380);	// FIXME 64-bit regs need multiple of 8 up to 32760
		return MemOperand(x28, offset);
	}

	void GenReadMemorySlow(u32 size)
	{
		Instruction *start_instruction = GetCursorAddress<Instruction *>();

		switch (size)
		{
		case 1:
			GenCallRuntime(addrspace::read8SX32);
			break;

		case 2:
			GenCallRuntime(addrspace::read16SX32);
			break;

		case 4:
			GenCallRuntime(addrspace::read32);
			break;

		case 8:
			GenCallRuntime(addrspace::read64);
			break;

		default:
			die("1..8 bytes");
			break;
		}
		EnsureCodeSize(start_instruction, read_memory_rewrite_size);
	}

	void GenWriteMemorySlow(u32 size)
	{
		Instruction *start_instruction = GetCursorAddress<Instruction *>();

		switch (size)
		{
		case 1:
			Uxtb(w1, w1);
			GenCallRuntime(addrspace::write8);
			break;

		case 2:
			Uxth(w1, w1);
			GenCallRuntime(addrspace::write16);
			break;

		case 4:
			GenCallRuntime(addrspace::write32);
			break;

		case 8:
			GenCallRuntime(addrspace::write64);
			break;

		default:
			die("1..8 bytes");
			break;
		}
		EnsureCodeSize(start_instruction, write_memory_rewrite_size);
	}

	void Mov_2ops(const Register& rd, uint64_t imm)
	{
		int ops = MoveImmediateHelper(this, rd, imm);
		verify(ops == 1 || ops == 2);
		if (ops == 1)
			Nop();
	}

	// Deadcast: a static block exit under the MMU (4 instructions, as the others): to the target block
	// when linked, which checks the pc it is entered with, else through the link stub when it may be
	// linked (mmu_canLink), else through the main loop.
	void GenMmuStaticExit(RuntimeBlockInfo *block, u32 target, RuntimeBlockInfo *linked, DynaCode *linkStub)
	{
		if (linked != nullptr)
		{
			Mov_2ops(w29, target);
			Str(w29, sh4_context_mem_operand(&sh4ctx.pc));
			// its pc check passes: entered past it
			GenBranch((DynaCode *)((u8 *)linked->code + (linked->vaddr == target ? linked->pcCheckSize : 0)));
		}
		else if (sh4ctx.CpuRunning && !block->mmuNoLink && mmu_canLink(block->vaddr, target))
		{
			GenCall(linkStub);
			Nop();
			Nop();
			Nop();
		}
		else
		{
			Mov_2ops(w29, target);
			Str(w29, sh4_context_mem_operand(&sh4ctx.pc));
			GenBranch(arm64_no_update);
		}
	}

	u32 RelinkBlock(RuntimeBlockInfo *block)
	{
		ptrdiff_t start_offset = GetBuffer()->GetCursorOffset();
		// The CpuRunning tests are needed when transitioning to disabled mmu
		switch (block->BlockType)
		{

		case BET_StaticJump:
		case BET_StaticCall:
			// next_pc = block->BranchBlock;
			if (mmu_enabled())
			{
				GenMmuStaticExit(block, block->BranchBlock, block->pBranchBlock, linkBlockGenericStub);
				break;
			}
#ifndef NO_BLOCK_LINKING
			if (block->pBranchBlock != NULL)
			{
				GenBranch((DynaCode *)block->pBranchBlock->code);
				Nop();
				Nop();
				Nop();
			}
			else
			{
				if (!mmu_enabled() && sh4ctx.CpuRunning)
				{
					GenCall(linkBlockGenericStub);
					Nop();
					Nop();
					Nop();
				}
				else
#else
			{
#endif
				{
					Mov_2ops(w29, block->BranchBlock);
					Str(w29, sh4_context_mem_operand(&sh4ctx.pc));
					GenBranch(arm64_no_update);
				}
			}
			break;

		case BET_Cond_0:
		case BET_Cond_1:
			{
				// next_pc = next_pc_value;
				// if (*jdyn == 0)
				//   next_pc = branch_pc_value;

				// Deadcast: the condition is in w29, from jcond or the block's end (compileBlock), or T itself in
				// w18 (exitTestsT). Taken when it is BlockType & 1 (T is 0 or 1)
				Label branch_not_taken;
				if (block->exitTestsT)
				{
					if (block->BlockType & 1)
						Cbz(w18, &branch_not_taken);
					else
						Cbnz(w18, &branch_not_taken);
				}
				else
				{
					Cmp(w29, block->BlockType & 1);
					B(ne, &branch_not_taken);
				}
				if (mmu_enabled())
				{
					GenMmuStaticExit(block, block->BranchBlock, block->pBranchBlock, linkBlockBranchStub);
					Bind(&branch_not_taken);
					GenMmuStaticExit(block, block->NextBlock, block->pNextBlock, linkBlockNextStub);
					break;
				}
#ifndef NO_BLOCK_LINKING
				if (block->pBranchBlock != NULL)
				{
					GenBranch((DynaCode *)block->pBranchBlock->code);
					Nop();
					Nop();
					Nop();
				}
				else
				{
					if (!mmu_enabled() && sh4ctx.CpuRunning)
					{
						GenCall(linkBlockBranchStub);
						Nop();
						Nop();
						Nop();
					}
					else
#else
				{
#endif
					{
						Mov_2ops(w29, block->BranchBlock);
						Str(w29, sh4_context_mem_operand(&sh4ctx.pc));
						GenBranch(arm64_no_update);
					}
				}

				Bind(&branch_not_taken);

#ifndef NO_BLOCK_LINKING
				if (block->pNextBlock != NULL)
				{
					GenBranch((DynaCode *)block->pNextBlock->code);
					Nop();
					Nop();
					Nop();
				}
				else
				{
					if (!mmu_enabled() && sh4ctx.CpuRunning)
					{
						GenCall(linkBlockNextStub);
						Nop();
						Nop();
						Nop();
					}
					else
#else
				{
#endif
					{
						Mov_2ops(w29, block->NextBlock);
						Str(w29, sh4_context_mem_operand(&sh4ctx.pc));
						GenBranch(arm64_no_update);
					}
				}
			}
			break;

		case BET_DynamicJump:
		case BET_DynamicCall:
		case BET_DynamicRet:
			// next_pc = *jdyn;

			Str(w29, sh4_context_mem_operand(&sh4ctx.pc));
			if (!mmu_enabled() && sh4ctx.CpuRunning)
			{
				ptrdiff_t start = GetBuffer()->GetCursorOffset();
				// TODO Call no_update instead (and check CpuRunning less frequently?)
				Sub(x2, x28, offsetof(Sh4RCB, cntx));	// 3 ops
				if (RAM_SIZE == 32_MB)
					Ubfx(w1, w29, 1, 24);
				else
					Ubfx(w1, w29, 1, 23);
				Ldr(x0, MemOperand(x2, x1, LSL, 3));	// Get block entry point
				Br(x0);
				// sadly no any easy way to check this at compile time
				verify(GetBuffer()->GetCursorOffset() - start == 6 * 4);
			}
			else if (mmu_enabled() && sh4ctx.CpuRunning)
			{
				// Deadcast: the main loop's lookup in mmuDispatchCache, right here: the branch to the next
				// block then is this exit's own, which the CPU predicts by itself
				Label slow;
				Tst(w29, 0x80000001);
				B(&slow, ne);
				if (mmuwin::current != nullptr)
					Sub(x2, x27, mmuwin::DispatchCacheOffset);	// the window's
				else
					Mov(x2, reinterpret_cast<uintptr_t>(mmuDispatchCache));
				Ubfx(w1, w29, 1, 12);
				Add(x2, x2, Operand(x1, LSL, 4));
				Ldr(w3, MemOperand(x2));
				Cmp(w3, w29);
				B(&slow, ne);
				Ldr(x0, MemOperand(x2, offsetof(MmuDispatchEntry, code)));
				Br(x0);
				Bind(&slow);
				GenBranch(arm64_no_update);
			}
			else
			{
				GenBranch(arm64_no_update);
				for (int i = 0 ; i < 5; i++)
					Nop();
			}

			break;

		case BET_DynamicIntr:
		case BET_StaticIntr:
			if (block->BlockType == BET_StaticIntr)
				// next_pc = next_pc_value;
				Mov(w29, block->NextBlock);
			// else next_pc = *jdyn (already in w29)

			Str(w29, sh4_context_mem_operand(&sh4ctx.pc));

			GenCallRuntime(UpdateINTC);

			Ldr(w29, sh4_context_mem_operand(&sh4ctx.pc));
			GenBranch(arm64_no_update);

			break;

		default:
			die("Invalid block end type");
		}

		return GetBuffer()->GetCursorOffset() - start_offset;
	}

	void Finalize(bool rewrite = false)
	{
		Label code_end;
		Bind(&code_end);

		FinalizeCode();

		if (!rewrite)
		{
			block->code = GetBuffer()->GetStartAddress<DynarecCodeEntryPtr>();
			block->host_code_size = GetBuffer()->GetSizeInBytes();
			block->host_opcodes = GetLabelAddress<u32*>(&code_end) - GetBuffer()->GetStartAddress<u32*>();

			codeBuffer.advance(block->host_code_size);
		}

		// Flush and invalidate caches
		virtmem::flush_cache(
			CC_RW2RX(GetBuffer()->GetStartAddress<void*>()), CC_RW2RX(GetBuffer()->GetEndAddress<void*>()),
			GetBuffer()->GetStartAddress<void*>(), GetBuffer()->GetEndAddress<void*>());
#if 0
		if (rewrite && block != NULL)
		{
			INFO_LOG(DYNAREC, "BLOCK %08x", block->vaddr);
			Instruction* instr_start = (Instruction*)block->code;
//			Instruction* instr_end = GetLabelAddress<Instruction*>(&code_end);
			Instruction* instr_end = (Instruction*)((u8 *)block->code + block->host_code_size);
			Decoder decoder;
			Disassembler disasm;
			decoder.AppendVisitor(&disasm);
			Instruction* instr;
			for (instr = instr_start; instr < instr_end; instr += kInstructionSize) {
				decoder.Decode(instr);
				INFO_LOG(DYNAREC, "VIXL  %p:  %s",
						   reinterpret_cast<void*>(instr),
						   disasm.GetOutput());
			}
		}
#endif
	}

	void GenMainloop()
	{
		Label no_update;
		Label intc_sched;
		Label end_mainloop;

		pinnedRegs = !mmu_enabled() && addrspace::window != nullptr;
		std::list<PendingThunk> thunks;
		Label callThunkLabel;
		Label lightThunkLabel;
		Label sqFlushLabel;
		if (pinnedRegs)
		{
			for (uintptr_t f : { reinterpret_cast<uintptr_t>(addrspace::read8SX32), reinterpret_cast<uintptr_t>(addrspace::read16SX32),
					reinterpret_cast<uintptr_t>(addrspace::read32), reinterpret_cast<uintptr_t>(addrspace::read64),
					reinterpret_cast<uintptr_t>(addrspace::write8), reinterpret_cast<uintptr_t>(addrspace::write16),
					reinterpret_cast<uintptr_t>(addrspace::write32), reinterpret_cast<uintptr_t>(addrspace::write64) })
			{
				thunks.emplace_back();
				thunks.back().function = f;
			}
			pendingThunks = &thunks;
			pendingCallThunk = &callThunkLabel;
		}

		// int arm64_intc_sched(u32 pc, int cycle_counter)
		arm64_intc_sched = GetCursorAddress<DynaCode *>();
		verify((void *)arm64_intc_sched == codeBuffer.getBase());
		B(&intc_sched);

		// Not yet compiled block stub
		// WARNING: this function must be at a fixed address, or transitioning to mmu will fail (switch)
		rdv_SetFailedToFindBlockHandler((void (*)())CC_RW2RX(GetCursorAddress<uintptr_t>()));
		if (mmu_enabled())
		{
			GenCallRuntime(rdv_FailedToFindBlock_pc);
		}
		else
		{
			Mov(w0, w29);
			GenCallRuntime(rdv_FailedToFindBlock);
		}
		Br(x0);

		//
		// void mainloop(void *context)
		//
		mainloop = (void (*)(void *))CC_RW2RX(GetCursorAddress<uintptr_t>());
		// For stack unwinding purposes, we pretend that the entire code block is just one function, with the same
		// unwinding instructions everywhere. This isn't true until the end of the following prolog, but exceptions
		// can only be thrown by called functions so this is good enough.
		unwinder.start(codeBuffer.getBase());

		// Save registers
		Stp(x19, x20, MemOperand(sp, -160, PreIndex));
		unwinder.allocStack(0, 160);
		unwinder.saveReg(0, x19, 160);
		unwinder.saveReg(0, x20, 152);
		Stp(x21, x22, MemOperand(sp, 16));
		unwinder.saveReg(0, x21, 144);
		unwinder.saveReg(0, x22, 136);
		Stp(x23, x24, MemOperand(sp, 32));
		unwinder.saveReg(0, x23, 128);
		unwinder.saveReg(0, x24, 120);
		Stp(x25, x26, MemOperand(sp, 48));
		unwinder.saveReg(0, x25, 112);
		unwinder.saveReg(0, x26, 104);
		Stp(x27, x28, MemOperand(sp, 64));
		unwinder.saveReg(0, x27, 96);
		unwinder.saveReg(0, x28, 88);
		Stp(d14, d15, MemOperand(sp, 80));
		unwinder.saveReg(0, d14, 80);
		unwinder.saveReg(0, d15, 72);
		Stp(d8, d9, MemOperand(sp, 96));
		unwinder.saveReg(0, d8, 64);
		unwinder.saveReg(0, d9, 56);
		Stp(d10, d11, MemOperand(sp, 112));
		unwinder.saveReg(0, d10, 48);
		unwinder.saveReg(0, d11, 40);
		Stp(d12, d13, MemOperand(sp, 128));
		unwinder.saveReg(0, d12, 32);
		unwinder.saveReg(0, d13, 24);
		Stp(x29, x30, MemOperand(sp, 144));
		unwinder.saveReg(0, x29, 16);
		unwinder.saveReg(0, x30, 8);

		Sub(x0, x0, sizeof(Sh4Context));
		Label reenterLabel;
		if (mmu_enabled())
		{
			// Push context
			Stp(x0, x1, MemOperand(sp, -16, PreIndex));
			unwinder.allocStack(0, 16);

			Ldr(x0, reinterpret_cast<uintptr_t>(&jmp_stack));
			Mov(x1, sp);
			Str(x1, MemOperand(x0));

			Bind(&reenterLabel);
			Ldr(x28, MemOperand(sp));	// Set context
			// Deadcast: the current ASID's window (deadcast/mmuwindows.h)
			Mov(x27, reinterpret_cast<uintptr_t>(&mmuwin::current));
			Ldr(x27, MemOperand(x27));
		}
		else
		{
			// Use x28 as sh4 context pointer
			Mov(x28, x0);
			// Deadcast: and x27 as the base of the dynarec's window (addrspace.h), else of the memory map,
			// right after the context (compact accesses)
			if (addrspace::window != nullptr)
				Mov(x27, reinterpret_cast<uintptr_t>(addrspace::window));
			else
				Add(x27, x28, sizeof(Sh4Context));
			if (pinnedRegs)
				genPinnedLoad();
		}
		Label do_interrupts;

		// w29 is next_pc
		Ldr(w29, MemOperand(x28, offsetof(Sh4Context, pc)));

		//
		// void arm64_no_update()
		//
		Bind(&no_update);				// next_pc _MUST_ be on w29

		Ldr(w0, MemOperand(x28, offsetof(Sh4Context, CpuRunning)));
		Cbz(w0, &end_mainloop);
		if (!mmu_enabled())
		{
			Sub(x2, x28, offsetof(Sh4RCB, cntx));
			if (RAM_SIZE == 32_MB)
				Ubfx(w1, w29, 1, 24);	// 24+1 bits: 32 MB
			else if (RAM_SIZE == 16_MB)
				Ubfx(w1, w29, 1, 23);	// 23+1 bits: 16 MB
			else
				die("Unsupported RAM_SIZE");
			Ldr(x0, MemOperand(x2, x1, LSL, 3));
		}
		else
		{
			// Deadcast: user-space pcs are looked up inline: in the dispatch cache, else through the code page
			// table and the block table, then cached. P1/P2 pcs go straight to the block table; odd pcs and
			// uncached pages take bm_GetCodeByVAddr.
			Label notCached, notInLut, notUser, keep;
			Tst(w29, 0x80000001);
			B(&notUser, ne);
			if (mmuwin::current != nullptr)
				Sub(x2, x27, mmuwin::DispatchCacheOffset);	// the window's (deadcast/mmuwindows.h)
			else
				Mov(x2, reinterpret_cast<uintptr_t>(mmuDispatchCache));
			Ubfx(w1, w29, 1, 12);
			static_assert(MmuDispatchEntries == 1 << 12 && sizeof(MmuDispatchEntry) == 16);
			Add(x2, x2, Operand(x1, LSL, 4));
			Ldr(w3, MemOperand(x2));
			Cmp(w3, w29);
			B(&notCached, ne);
			Ldr(x0, MemOperand(x2, offsetof(MmuDispatchEntry, code)));
			Br(x0);

			Bind(&notCached);	// x2: the entry for this pc
			Lsr(w1, w29, 12);
			if (mmuwin::current != nullptr)
			{
				// slot 0 in the window's own table (deadcast/mmuwindows.h), the rest in mmuAddressLUT
				Label global, lookUp;
				Cmp(w1, MmuSlot0Pages);
				B(&global, hs);
				Sub(x3, x27, mmuwin::CodeLutOffset);
				B(&lookUp);
				Bind(&global);
				Mov(x3, reinterpret_cast<uintptr_t>(mmuAddressLUT));
				Bind(&lookUp);
			}
			else
				Mov(x3, reinterpret_cast<uintptr_t>(mmuAddressLUT));
			Ldr(x1, MemOperand(x3, x1, LSL, 3));
			Cbz(x1, &notInLut);
			// the physical address: the host address less the memory map's base
			Add(x1, x1, Operand(w29, UXTW));
			Mov(x3, reinterpret_cast<uintptr_t>(addrspace::ram_base));
			Sub(x1, x1, x3);
			Sub(x3, x28, offsetof(Sh4RCB, cntx));
			if (RAM_SIZE == 32_MB)
				Ubfx(w1, w1, 1, 24);
			else if (RAM_SIZE == 16_MB)
				Ubfx(w1, w1, 1, 23);
			else
				die("Unsupported RAM_SIZE");
			Ldr(x0, MemOperand(x3, x1, LSL, 3));
			// a block not compiled yet is not kept: its code is about to change
			Mov(x3, reinterpret_cast<uintptr_t>(ngen_FailedToFindBlock));
			Cmp(x0, x3);
			B(&keep, eq);
			Str(w29, MemOperand(x2));
			Str(x0, MemOperand(x2, offsetof(MmuDispatchEntry, code)));
			// Deadcast: noted, for a flush to empty this one only (mmuDispatchCacheFlush)
			{
				Label full;
				if (mmuwin::current != nullptr)
					Sub(x3, x27, mmuwin::DispatchFilledOffset);
				else
					Mov(x3, reinterpret_cast<uintptr_t>(&mmuDispatchFilled));
				Ldr(w1, MemOperand(x3, offsetof(MmuFilled, count)));
				Cmp(w1, MmuFilled::Max);
				B(&full, hs);
				Ubfx(w4, w29, 1, 12);
				Add(x5, x3, offsetof(MmuFilled, index));
				Str(w4, MemOperand(x5, x1, LSL, 2));
				Bind(&full);
				Add(w1, w1, 1);
				Str(w1, MemOperand(x3, offsetof(MmuFilled, count)));
			}
			Bind(&keep);
			Br(x0);

			// Deadcast: P1 and P2 pcs, untranslated, straight from the FPCB as without the MMU
			Bind(&notUser);
			And(w1, w29, 0xC0000001);
			Cmp(w1, 0x80000000);
			B(&notInLut, ne);
			Sub(x2, x28, offsetof(Sh4RCB, cntx));
			if (RAM_SIZE == 32_MB)
				Ubfx(w1, w29, 1, 24);
			else
				Ubfx(w1, w29, 1, 23);
			Ldr(x0, MemOperand(x2, x1, LSL, 3));
			Br(x0);

			Bind(&notInLut);
			Mov(w0, w29);
			GenCallRuntime(bm_GetCodeByVAddr);
		}
		Br(x0);

		Bind(&end_mainloop);
		if (pinnedRegs)
			genPinnedStore();
		if (mmu_enabled())
			// Pop context
			Add(sp, sp, 16);
		// Restore registers
		Ldp(x29, x30, MemOperand(sp, 144));
		Ldp(d12, d13, MemOperand(sp, 128));
		Ldp(d10, d11, MemOperand(sp, 112));
		Ldp(d8, d9, MemOperand(sp, 96));
		Ldp(d14, d15, MemOperand(sp, 80));
		Ldp(x27, x28, MemOperand(sp, 64));
		Ldp(x25, x26, MemOperand(sp, 48));
		Ldp(x23, x24, MemOperand(sp, 32));
		Ldp(x21, x22, MemOperand(sp, 16));
		Ldp(x19, x20, MemOperand(sp, 160, PostIndex));
		Ret();

		//
		// int arm64_intc_sched(u32 pc, u32 cycle_counter)
		//
		Bind(&intc_sched);	// w0 is pc, w1 is cycle_counter

		Str(w0, sh4_context_mem_operand(&sh4ctx.pc));
		// Add timeslice to cycle counter
		Add(w1, w1, SH4_TIMESLICE);
		Str(w1, sh4_context_mem_operand(&sh4ctx.cycle_counter));
		Ldr(w0, sh4_context_mem_operand(&sh4ctx.CpuRunning));
		Cbz(w0, &end_mainloop);
		// Deadcast: UpdateSystem_INTC's common case right here, no call: the timeslice taken off
		// sh4_sched_next, no event due and no interrupt pending
		Label due;
		Ldr(w2, sh4_context_mem_operand(&sh4ctx.sh4_sched_next));
		Subs(w2, w2, SH4_TIMESLICE);
		Str(w2, sh4_context_mem_operand(&sh4ctx.sh4_sched_next));
		B(&due, mi);
		Ldr(w2, sh4_context_mem_operand(&sh4ctx.interrupt_pend));
		Cbnz(w2, &due);
		Mov(w0, w1);
		Ret();
		Bind(&due);
		Mov(x29, lr);				// Save link register in case we return
		GenCallRuntime(UpdateSystem_INTC_due);
		Cbnz(w0, &do_interrupts);
		Mov(lr, x29);
		Ldr(w0, sh4_context_mem_operand(&sh4ctx.cycle_counter));
		Ret();

		Bind(&do_interrupts);
		Ldr(w29, sh4_context_mem_operand(&sh4ctx.pc));
		B(&no_update);

		//
		// Exception handler
		//
		Label handleExceptionLabel;
		Bind(&handleExceptionLabel);
		if (mmu_enabled())
		{
			Ldr(x0, reinterpret_cast<uintptr_t>(&jmp_stack));
			Ldr(x1, MemOperand(x0));
			Mov(sp, x1);
			B(&reenterLabel);
		}

		// MMU Block check: SR.FD == 1 (FPU enabled)
		// w0: vaddr
		checkBlockFpu = GetCursorAddress<DynaCode *>();
		Label fpu_enabled;
		Ldr(w3, sh4_context_mem_operand(&sh4ctx.sr.status));
		Tbz(w3, 15, &fpu_enabled);			// test SR.FD bit

		Mov(w1, Sh4Ex_FpuDisabled);	// exception code
		GenCallRuntime(Do_Exception);
		Ldr(w29, sh4_context_mem_operand(&sh4ctx.pc));
		B(&no_update);
		Bind(&fpu_enabled);
		Ret();

		// MMU Block check: pc == vaddr
		// w0: vaddr, w1: addr
		checkBlockAddr = GetCursorAddress<DynaCode *>();
		Ldr(w2, sh4_context_mem_operand(&sh4ctx.pc));
		Cmp(w2, w0);
		Mov(w0, w1);
		Label blockCheckFailLabel;
		B(&blockCheckFailLabel, ne);
		Ret();

		// Block check fail
		// w0: addr
		Bind(&blockCheckFailLabel);
		GenCallRuntime(rdv_BlockCheckFail);
		if (mmu_enabled())
		{
			Label jumpblockLabel;
			Cbnz(x0, &jumpblockLabel);
			Ldr(w0, sh4_context_mem_operand(&sh4ctx.pc));
			GenCallRuntime(bm_GetCodeByVAddr);
			Bind(&jumpblockLabel);
		}
		Br(x0);

		// Block linking stubs
		linkBlockBranchStub = GetCursorAddress<DynaCode *>();
		Label linkBlockShared;
		Mov(w1, 1);
		B(&linkBlockShared);

		linkBlockNextStub = GetCursorAddress<DynaCode *>();
		Mov(w1, 0);
		B(&linkBlockShared);

		linkBlockGenericStub = GetCursorAddress<DynaCode *>();
		Mov(w1, w29);	// djump/pc -> in case we need it ..

		Bind(&linkBlockShared);
		Sub(x0, lr, 4);	// go before the call
		GenCallRuntime(rdv_LinkBlock);	// returns an RX addr
		Br(x0);

		// Store Queue write handlers
		Label writeStoreQueue32Label;
		Bind(&writeStoreQueue32Label);
		Lsr(x2, x0, 26);
		Cmp(x2, 0x38);
		GenBranchRuntime(addrspace::write32, Condition::ne);
		And(x0, x0, 0x3f);
		Str(w1, MemOperand(x28, x0));
		Ret();

		Label writeStoreQueue64Label;
		Bind(&writeStoreQueue64Label);
		Lsr(x2, x0, 26);
		Cmp(x2, 0x38);
		GenBranchRuntime(addrspace::write64, Condition::ne);
		And(x0, x0, 0x3f);
		Str(x1, MemOperand(x28, x0));
		Ret();

		if (pinnedRegs)
		{
			// Deadcast: a store queue's flush, w0 the address (pref, in the store queues' area). To the
			// TA's polygon FIFO (sqTaRegionsUpdate), as ta_vtx_data32 does: the 32 bytes copied to the
			// TA's data, a step of its state machine, its handler called only for a transition to
			// handle. Anything else, or the TA's data at its start or full, through the handler.
			Bind(&sqFlushLabel);
			{
				Label slow, handle;
				Ubfx(w1, w0, 20, 6);
				Add(x2, x28, SqTaRegionsOffset);
				Ldrb(w2, MemOperand(x2, x1));
				Cbz(w2, &slow);
				Mov(x2, reinterpret_cast<uintptr_t>(taContext));
				Ldr(x2, MemOperand(x2));
				Cbz(x2, &slow);
				Mov(x2, reinterpret_cast<uintptr_t>(taDataWrite));
				Ldr(x3, MemOperand(x2));
				Ldr(x16, MemOperand(x2, taDataRootOffset));
				Sub(x17, x3, x16);
				Cbz(x17, &slow);
				Mov(x16, taDataSize - 64);
				Cmp(x17, x16);
				B(&slow, hs);
				// the store queue: sq_buffer, at the context's start
				Ubfx(w1, w0, 5, 1);
				Add(x1, x28, Operand(x1, LSL, 5));
				Ldp(q0, q1, MemOperand(x1));
				Stp(q0, q1, MemOperand(x3));
				Add(x3, x3, 32);
				Str(x3, MemOperand(x2));
				// state_in = (state << 8) | (pcw.ParaType << 5) | ((pcw.obj_ctrl >> 2) & 31)
				Ldr(w16, MemOperand(x1));
				Mov(x17, reinterpret_cast<uintptr_t>(ta_fsm));
				Ldrb(w3, MemOperand(x17, 2048));
				Lsr(w2, w16, 29);
				Ubfx(w16, w16, 2, 5);
				Orr(w16, w16, Operand(w2, LSL, 5));
				Orr(w16, w16, Operand(w3, LSL, 8));
				Ldrb(w3, MemOperand(x17, x16));
				Strb(w3, MemOperand(x17, 2048));
				Tst(w3, 0xF0);
				B(&handle, ne);
				Ret();
				Bind(&handle);
				// tail calls, through the light thunk
				Mov(w0, w3);
				Mov(x16, reinterpret_cast<uintptr_t>(ta_vtx_handle));
				B(&lightThunkLabel);
				Bind(&slow);
				Mov(x1, x28);
				Ldr(x16, sh4_context_mem_operand(&sh4ctx.doSqWrite));
				B(&lightThunkLabel);
			}

			pendingThunks = nullptr;
			pendingCallThunk = nullptr;
			Bind(&callThunkLabel);
			genThunk(0);
			Bind(&lightThunkLabel);
			genLightThunk(0);
			for (PendingThunk& thunk : thunks)
			{
				Bind(&thunk.label);
				genLightThunk(thunk.function);
			}
		}

		FinalizeCode();
		codeBuffer.advance(GetBuffer()->GetSizeInBytes());

		size_t unwindSize = unwinder.end(codeBuffer.getSize() - 128, (ptrdiff_t)CC_RW2RX(0));
		verify(unwindSize <= 128);

		arm64_no_update = GetLabelAddress<DynaCode *>(&no_update);
		handleException = (void (*)())CC_RW2RX(GetLabelAddress<uintptr_t>(&handleExceptionLabel));
		blockCheckFail = GetLabelAddress<DynaCode *>(&blockCheckFailLabel);
		writeStoreQueue32 = GetLabelAddress<DynaCode *>(&writeStoreQueue32Label);
		writeStoreQueue64 = GetLabelAddress<DynaCode *>(&writeStoreQueue64Label);
		callThunks.clear();
		callThunk = nullptr;
		lightThunk = nullptr;
		sqFlush = nullptr;
		if (pinnedRegs)
		{
			callThunk = GetLabelAddress<DynaCode *>(&callThunkLabel);
			lightThunk = GetLabelAddress<DynaCode *>(&lightThunkLabel);
			sqFlush = GetLabelAddress<DynaCode *>(&sqFlushLabel);
			for (PendingThunk& thunk : thunks)
				callThunks.emplace_back(thunk.function, GetLabelAddress<DynaCode *>(&thunk.label));
		}

		// Flush and invalidate caches
		virtmem::flush_cache(
			CC_RW2RX(GetBuffer()->GetStartAddress<void*>()), CC_RW2RX(GetBuffer()->GetEndAddress<void*>()),
			GetBuffer()->GetStartAddress<void*>(), GetBuffer()->GetEndAddress<void*>());
	}

	// Deadcast: what a faulting direct access becomes (Arm64Dynarec::rewrite): a handler call with the
	// address in w0 and data in w1, a read's result moved to the load's register. It replaces the
	// compact access's instructions (compact_read_size, compact_write_size).
	void GenMemoryRewrite(bool read, u32 size, bool fp, u32 addrReg, u32 dataReg, bool storeQueue)
	{
		Instruction *start_instruction = GetCursorAddress<Instruction *>();
		if (read)
		{
			if (addrReg != 0)
				Mov(w0, Register(addrReg, 32));
			switch (size)
			{
			case 1:
				GenCallRuntime(addrspace::read8SX32);
				break;
			case 2:
				GenCallRuntime(addrspace::read16SX32);
				break;
			case 4:
				GenCallRuntime(addrspace::read32);
				break;
			default:
				GenCallRuntime(addrspace::read64);
				break;
			}
			if (fp)
				Fmov(VRegister(dataReg, 32), w0);
			else if (dataReg != 0)
				Mov(Register(dataReg, 32), w0);
			verify(GetCursorAddress<Instruction *>() - start_instruction <= compact_read_size * kInstructionSize);
			EnsureCodeSize(start_instruction, compact_read_size);
		}
		else
		{
			// the data is never in w0 nor the address in w1 (GenWriteMemory): no move clobbers the other
			verify(addrReg != 1 && (fp || dataReg != 0));
			if (fp)
				Fmov(w1, VRegister(dataReg, 32));
			else if (dataReg != 1)
				Mov(w1, Register(dataReg, 32));
			if (addrReg != 0)
				Mov(w0, Register(addrReg, 32));
			if (storeQueue && size >= 4)
				GenCall(size == 4 ? writeStoreQueue32 : writeStoreQueue64);
			else
			{
				switch (size)
				{
				case 1:
					Uxtb(w1, w1);
					GenCallRuntime(addrspace::write8);
					break;
				case 2:
					Uxth(w1, w1);
					GenCallRuntime(addrspace::write16);
					break;
				case 4:
					GenCallRuntime(addrspace::write32);
					break;
				default:
					GenCallRuntime(addrspace::write64);
					break;
				}
			}
			verify(GetCursorAddress<Instruction *>() - start_instruction <= compact_write_size * kInstructionSize);
			EnsureCodeSize(start_instruction, compact_write_size);
		}
	}

	void GenWriteStoreQueue(u32 size)
	{
		Instruction *start_instruction = GetCursorAddress<Instruction *>();

		if (size == 4)
			GenCall(writeStoreQueue32);
		else
			GenCall(writeStoreQueue64);
		EnsureCodeSize(start_instruction, write_memory_rewrite_size);
	}

private:
	// Runtime branches/calls need to be adjusted if rx space is different to rw space.
	// Therefore can't mix GenBranch with GenBranchRuntime!

	template <typename R, typename... P>
	void GenCallRuntime(R (*function)(P...))
	{
		if (pinnedRegs && !inThunk)
		{
			genThunkCall(reinterpret_cast<uintptr_t>(function), al, true);
			return;
		}
		ptrdiff_t offset = reinterpret_cast<uintptr_t>(function) - reinterpret_cast<uintptr_t>(CC_RW2RX(GetBuffer()->GetStartAddress<void*>()));
		verify((offset & 3) == 0);
		if (offset < -128 * 1024 * 1024 || offset > 128 * 1024 * 1024)
		{
			Mov(x16, reinterpret_cast<uintptr_t>(function));
			Blr(x16);
		}
		else
		{
			Label function_label;
			BindToOffset(&function_label, offset);
			Bl(&function_label);
		}
	}

	void GenCall(DynaCode *function)
	{
		ptrdiff_t offset = reinterpret_cast<uintptr_t>(function) - GetBuffer()->GetStartAddress<uintptr_t>();
		verify(offset >= -128 * 1024 * 1024 && offset <= 128 * 1024 * 1024);
		verify((offset & 3) == 0);
		Label function_label;
		BindToOffset(&function_label, offset);
		Bl(&function_label);
	}

   template <typename R, typename... P>
	void GenBranchRuntime(R (*target)(P...), Condition cond = al)
	{
		if (pinnedRegs && !inThunk)
		{
			genThunkCall(reinterpret_cast<uintptr_t>(target), cond, false);
			return;
		}
		ptrdiff_t offset = reinterpret_cast<uintptr_t>(target) - reinterpret_cast<uintptr_t>(CC_RW2RX(GetBuffer()->GetStartAddress<void*>()));
		verify((offset & 3) == 0);
		if (offset < -128 * 1024 * 1024 || offset > 128 * 1024 * 1024)
		{
			if (cond == al)
			{
				Mov(x16, reinterpret_cast<uintptr_t>(target));
				Br(x16);
			}
			else
			{
				Label skip_target;
				Condition inverse_cond = (Condition)((u32)cond ^ 1);
				
				B(&skip_target, inverse_cond);
				Mov(x16, reinterpret_cast<uintptr_t>(target));
				Br(x16);
				Bind(&skip_target);
			}
		}
		else
		{
			Label target_label;
			BindToOffset(&target_label, offset);
			if (cond == al)
				B(&target_label);
			else
				B(&target_label, cond);
		}
	}

	// Deadcast: a C call while R0-R15 and T are pinned (pinnedRegs), through a thunk that writes them to
	// the context, keeps w4-w7, calls, and reloads them (the call may change them). A branch to a thunk is
	// a tail call; only functions with a thunk of their own can be branched to.
	void genThunkCall(uintptr_t function, Condition cond, bool call)
	{
		if (pendingThunks != nullptr)
		{
			for (PendingThunk& thunk : *pendingThunks)
				if (thunk.function == function)
				{
					if (call)
						Bl(&thunk.label);
					else
						B(&thunk.label, cond);
					return;
				}
			verify(call);
			Mov(x16, function);
			Bl(pendingCallThunk);
			return;
		}
		for (const auto& [f, thunk] : callThunks)
			if (f == function)
			{
				if (call)
					GenCall(thunk);
				else
					GenBranch(thunk, cond);
				return;
			}
		verify(call);
		Mov(x16, function);
		GenCall(callThunk);
	}

	// Deadcast: the pinned registers to and from the context
	void genPinnedStore()
	{
		for (int i = 0; i < 16; i += 2)
			Stp(Register(pinned_gregs[i], 32), Register(pinned_gregs[i + 1], 32), sh4_context_mem_operand((Sh4RegType)(reg_r0 + i)));
		Str(Register(pinned_T, 32), sh4_context_mem_operand(reg_sr_T));
	}
	void genPinnedLoad()
	{
		for (int i = 0; i < 16; i += 2)
			Ldp(Register(pinned_gregs[i], 32), Register(pinned_gregs[i + 1], 32), sh4_context_mem_operand((Sh4RegType)(reg_r0 + i)));
		Ldr(Register(pinned_T, 32), sh4_context_mem_operand(reg_sr_T));
	}

	// a light thunk: function's, else lightThunk, which calls x16. It keeps the host registers C may
	// change (the pinned R8-R15 and T, w4-w7), on the stack; R0-R7 are in registers C keeps.
	void genLightThunk(uintptr_t function)
	{
		inThunk = true;
		Stp(x4, x5, MemOperand(sp, -112, PreIndex));
		Stp(x6, x7, MemOperand(sp, 16));
		Stp(x8, x9, MemOperand(sp, 32));
		Stp(x10, x11, MemOperand(sp, 48));
		Stp(x12, x13, MemOperand(sp, 64));
		Stp(x14, x15, MemOperand(sp, 80));
		Stp(x18, x30, MemOperand(sp, 96));
		if (function == 0)
			Blr(x16);
		else
			GenCallRuntime((void (*)())function);
		Ldp(x18, x30, MemOperand(sp, 96));
		Ldp(x14, x15, MemOperand(sp, 80));
		Ldp(x12, x13, MemOperand(sp, 64));
		Ldp(x10, x11, MemOperand(sp, 48));
		Ldp(x8, x9, MemOperand(sp, 32));
		Ldp(x6, x7, MemOperand(sp, 16));
		Ldp(x4, x5, MemOperand(sp, 112, PostIndex));
		Ret();
		inThunk = false;
	}

	// a thunk (genThunkCall): function's, else callThunk, which calls x16
	void genThunk(uintptr_t function)
	{
		inThunk = true;
		const int save = offsetof(Sh4Context, jitSave);
		Str(x30, MemOperand(x28, save));
		Stp(x4, x5, MemOperand(x28, save + 16));
		Stp(x6, x7, MemOperand(x28, save + 32));
		genPinnedStore();
		if (function == 0)
			Blr(x16);
		else
			GenCallRuntime((void (*)())function);
		genPinnedLoad();
		Ldp(x6, x7, MemOperand(x28, save + 32));
		Ldp(x4, x5, MemOperand(x28, save + 16));
		Ldr(x30, MemOperand(x28, save));
		Ret();
		inThunk = false;
	}

	void GenBranch(DynaCode *code, Condition cond = al)
	{
		ptrdiff_t offset = reinterpret_cast<uintptr_t>(code) - GetBuffer()->GetStartAddress<uintptr_t>();
		verify(offset >= -128 * 1024 * 1024 && offset < 128 * 1024 * 1024);
		verify((offset & 3) == 0);
		Label code_label;
		BindToOffset(&code_label, offset);
		if (cond == al)
			B(&code_label);
		else
			B(&code_label, cond);
	}

	// Deadcast: the store queue writes whose address raised an exception (shop_pref), at the end of the
	// block; a list, as the labels must stay where they are
	struct PrefSlowPath
	{
		Label entry;
		Label back;
		shil_param addr;
		u32 pc;
		std::vector<Arm64RegAlloc::DirtyReg> dirty;
	};
	std::list<PrefSlowPath> prefSlowPaths;

	void genPrefSlowPaths()
	{
		if (prefSlowPaths.empty())
			return;
		Label done;
		B(&done);
		for (PrefSlowPath& slow : prefSlowPaths)
		{
			Bind(&slow.entry);
			genWriteBackDirty(slow.dirty);
			// the address from the context, complete now
			if (slow.addr.is_imm())
				Mov(w0, slow.addr._imm);
			else
				Ldr(w0, sh4_context_mem_operand(slow.addr._reg));
			Mov(x1, x28);
			Mov(w2, slow.pc);
			GenCallRuntime(do_sqw_mmu_no_ex);
			B(&slow.back);
		}
		Bind(&done);
		prefSlowPaths.clear();
	}

	// Deadcast: an MMU access through the current ASID's window (x27): one load or store, [x27, vaddr].
	// Faulting RAM is mapped by the fault handler; anything else goes to the access's stub at the end of
	// the block (genMmuAccessStubs), which writes back registers, calls mmuRead or mmuWrite and reloads
	// x27 after a write (the ASID may change).
	struct MmuAccess
	{
		Label stub;
		Label next;
		u32 accessOffset = 0;
		Register vaddr;
		u32 write;
		u32 pc;
		u32 site;		// its instruction's SH4 address (sqStoreSites)
		std::vector<Arm64RegAlloc::DirtyReg> dirty;	// to write back first (regalloc.mmuDirty)
		u32 size = 0;
		CPURegister data;
	};
	std::list<MmuAccess> mmuAccesses;

	// Deadcast: the accesses go through a window, [x27, address]: the MMU's ASID's, or without the MMU the
	// dynarec's (addrspace.h)
	bool windowAccesses() const {
		return mmu_enabled() || addrspace::window != nullptr;
	}

	MmuAccess& genMmuAccess(const shil_opcode& op, u32 write, const Register& vaddr)
	{
		mmuAccesses.emplace_back();
		MmuAccess& access = mmuAccesses.back();
		access.vaddr = vaddr;
		access.write = write;
		access.pc = block->vaddr + op.guest_offs - (op.delay_slot ? 2 : 0);
		access.site = block->vaddr + op.guest_offs;
		access.dirty = regalloc.mmuDirty;
		return access;
	}

	void genReloadWindow()
	{
		Mov(x9, reinterpret_cast<uintptr_t>(&mmuwin::current));
		Ldr(x27, MemOperand(x9));
	}

	void genMmuAccessStubs()
	{
		if (mmuAccesses.empty())
			return;
		Label done;
		B(&done);
		for (MmuAccess& access : mmuAccesses)
		{
			Bind(&access.stub);
			if (access.write && access.size >= 4)
			{
				// the store queues (vertices for the TA), written here: sq_buffer, at the context's start
				Label notStoreQueue;
				Lsr(w3, access.vaddr, 26);
				Cmp(w3, 0x38);
				B(&notStoreQueue, ne);
				And(x3, access.vaddr.X(), 0x3f);
				if (access.size == 8)
					Str(x1, MemOperand(x28, x3));
				else if (access.data.IsVRegister())
					Str(VRegister(access.data.GetCode(), 32), MemOperand(x28, x3));
				else
					Str(Register(access.data.GetCode(), 32), MemOperand(x28, x3));
				B(&access.next);
				Bind(&notStoreQueue);
			}
			if (!mmu_enabled())
			{
				// Deadcast: without the MMU, what the window does not map (registers, the 32-bit VRAM
				// path, AICA RAM written): its handler, no exception to raise
				if (access.write)
				{
					if (access.size == 8)
						Mov(x2, x1);
					else if (access.data.IsVRegister())
						Fmov(w2, VRegister(access.data.GetCode(), 32));
					else
						Mov(w2, Register(access.data.GetCode(), 32));
					if (!access.vaddr.Is(w0))
						Mov(w0, access.vaddr);
					switch (access.size)
					{
					case 1:
						Uxtb(w1, w2);
						GenCallRuntime(addrspace::write8);
						break;
					case 2:
						Uxth(w1, w2);
						GenCallRuntime(addrspace::write16);
						break;
					case 4:
						Mov(w1, w2);
						GenCallRuntime(addrspace::write32);
						break;
					default:
						Mov(x1, x2);
						GenCallRuntime(addrspace::write64);
						break;
					}
				}
				else
				{
					if (!access.vaddr.Is(w0))
						Mov(w0, access.vaddr);
					switch (access.size)
					{
					case 1:
						GenCallRuntime(addrspace::read8SX32);
						break;
					case 2:
						GenCallRuntime(addrspace::read16SX32);
						break;
					case 4:
						GenCallRuntime(addrspace::read32);
						break;
					default:
						GenCallRuntime(addrspace::read64);
						break;
					}
					if (access.data.IsVRegister())
						Fmov(VRegister(access.data.GetCode(), 32), w0);
					else if (access.size != 8 && access.data.GetCode() != 0)
						Mov(Register(access.data.GetCode(), 32), w0);
				}
				B(&access.next);
				block->mmuStubs.emplace_back(access.accessOffset, (u32)access.stub.GetLocation());
				block->mmuSites.push_back(access.site);
				continue;
			}
			genWriteBackDirty(access.dirty);
			if (access.write)
			{
				// the data first, from w1 maybe (GenWriteMemory), which takes the pc
				if (access.size == 8)
					Mov(x2, x1);
				else if (access.data.IsVRegister())
					Fmov(w2, VRegister(access.data.GetCode(), 32));
				else
					Mov(w2, Register(access.data.GetCode(), 32));
				if (!access.vaddr.Is(w0))
					Mov(w0, access.vaddr);
				Mov(w1, access.pc);
				switch (access.size)
				{
				case 1:
					GenCallRuntime(mmuWrite8);
					break;
				case 2:
					GenCallRuntime(mmuWrite16);
					break;
				case 4:
					GenCallRuntime(mmuWrite32);
					break;
				default:
					GenCallRuntime(mmuWrite64);
					break;
				}
				genReloadWindow();
			}
			else
			{
				if (!access.vaddr.Is(w0))
					Mov(w0, access.vaddr);
				Mov(w1, access.pc);
				switch (access.size)
				{
				case 1:
					GenCallRuntime(mmuRead8);
					break;
				case 2:
					GenCallRuntime(mmuRead16);
					break;
				case 4:
					GenCallRuntime(mmuRead32);
					break;
				default:
					GenCallRuntime(mmuRead64);
					break;
				}
				if (access.data.IsVRegister())
					Fmov(VRegister(access.data.GetCode(), 32), w0);
				else if (access.size != 8 && access.data.GetCode() != 0)
					Mov(Register(access.data.GetCode(), 32), w0);
			}
			B(&access.next);
			block->mmuStubs.emplace_back(access.accessOffset, (u32)access.stub.GetLocation());
			block->mmuSites.push_back(access.site);
		}
		Bind(&done);
		mmuAccesses.clear();
	}

	void genWriteBackDirty(const std::vector<Arm64RegAlloc::DirtyReg>& dirty)
	{
		for (const auto& d : dirty)
		{
			if (d.isFloat)
				Str(VRegister(d.host_reg, 32), sh4_context_mem_operand(d.reg));
			else
				Str(Register(d.host_reg, 32), sh4_context_mem_operand(d.reg));
		}
	}

	void genMmuLookup(const shil_opcode& op, u32 write)
	{
		if (mmu_enabled())
		{
			Label inCache;
			Label done;

			// the kernel's half is not translated, as the page cache has it (P1, P2, P4)
			Tbnz(w0, 31, &done);
			Lsr(w1, w0, 12);
			Mov(x2, reinterpret_cast<uintptr_t>(mmuAddressLUT));
			Ldr(x1, MemOperand(x2, x1, LSL, 3));
			Cbnz(x1, &inCache);
			genWriteBackDirty(regalloc.mmuDirty);
			Mov(w1, write);
			Mov(w2, block->vaddr + op.guest_offs - (op.delay_slot ? 2 : 0));	// pc
			GenCallRuntime(mmuDynarecLookup);
			B(&done);
			Bind(&inCache);
			// the physical address: the host address less the memory map's base
			Add(x1, x1, Operand(w0, UXTW));
			Mov(x2, reinterpret_cast<uintptr_t>(addrspace::ram_base));
			Sub(x0, x1, x2);
			Bind(&done);
		}
	}

	// Deadcast: whether a memory access can take the compact form (GenReadMemory, GenWriteMemory): the
	// handlers its rewrite calls must be in reach of a single bl
	bool compactMemAccess(const shil_opcode& op, bool optimise)
	{
		if (!optimise || !addrspace::virtmemEnabled() || (mmu_enabled() && mmuwin::current == nullptr))
			return false;
		if (pinnedRegs)
			return true;	// the calls go through the thunks, at hand
		const uintptr_t here = reinterpret_cast<uintptr_t>(CC_RW2RX(GetBuffer()->GetStartAddress<void*>()));
		for (uintptr_t f : { reinterpret_cast<uintptr_t>(addrspace::read8SX32), reinterpret_cast<uintptr_t>(addrspace::read16SX32),
				reinterpret_cast<uintptr_t>(addrspace::read32), reinterpret_cast<uintptr_t>(addrspace::read64),
				reinterpret_cast<uintptr_t>(addrspace::write8), reinterpret_cast<uintptr_t>(addrspace::write16),
				reinterpret_cast<uintptr_t>(addrspace::write32), reinterpret_cast<uintptr_t>(addrspace::write64) })
		{
			ptrdiff_t offset = f - here;
			if (offset < -120 * 1024 * 1024 || offset > 120 * 1024 * 1024)
				return false;
		}
		return true;
	}

	void GenReadMemory(const shil_opcode& op, size_t opid, bool optimise)
	{
		if (GenReadMemoryImmediate(op))
			return;

		if (compactMemAccess(op, optimise))
		{
			// Deadcast: the address from the register it is in, the value loaded straight into its
			// register: under the MMU through the window (genMmuAccess), else ubfx and load, which
			// rewrite() turns into a handler call (GenMemoryRewrite).
			const Register& raddr = GenMemAddr(op);
			MmuAccess *mmu = windowAccesses() ? &genMmuAccess(op, 0, raddr) : nullptr;
			CPURegister dest = op.size == 8 ? CPURegister(x0) : CPURegister(w0);
			if (op.size != 8 && regalloc.IsAllocg(op.rd))
				dest = regalloc.MapRegister(op.rd);
			else if (op.size == 4 && regalloc.IsAllocf(op.rd))
				dest = regalloc.MapVRegister(op.rd);
			if (mmu != nullptr)
			{
				const MemOperand addr(x27, raddr, UXTW);
				mmu->accessOffset = (u32)GetBuffer()->GetCursorOffset();
				switch (op.size)
				{
				case 1:
					Ldrsb(dest.W(), addr);
					break;
				case 2:
					Ldrsh(dest.W(), addr);
					break;
				case 4:
					if (dest.IsVRegister())
						Ldr(dest.S(), addr);
					else
						Ldr(dest.W(), addr);
					break;
				case 8:
					Ldr(x0, addr);
					break;
				}
				Bind(&mmu->next);
				mmu->size = op.size;
				mmu->data = dest;
				if (dest.Is(w0) || dest.Is(x0))
					host_reg_to_shil_param(op.rd, dest);
				return;
			}
			Instruction *start_instruction = GetCursorAddress<Instruction *>();
			// without the MMU, x27 is the memory map's base
			Ubfx(x1, raddr.X(), 0, 29);
			switch (op.size)
			{
			case 1:
				Ldrsb(dest.W(), MemOperand(x27, x1));
				break;
			case 2:
				Ldrsh(dest.W(), MemOperand(x27, x1));
				break;
			case 4:
				if (dest.IsVRegister())
					Ldr(dest.S(), MemOperand(x27, x1));
				else
					Ldr(dest.W(), MemOperand(x27, x1));
				break;
			case 8:
				Ldr(x0, MemOperand(x27, x1));
				break;
			}
			EnsureCodeSize(start_instruction, compact_read_size);
			if (dest.Is(w0) || dest.Is(x0))
				host_reg_to_shil_param(op.rd, dest);
			return;
		}

		GenMemAddr(op, &w0);
		genMmuLookup(op, 0);

		if (!optimise || !GenReadMemoryFast(op, opid))
			GenReadMemorySlow(op.size);

		if (op.size < 8)
			host_reg_to_shil_param(op.rd, w0);
		else
			host_reg_to_shil_param(op.rd, x0);
	}

	bool GenReadMemoryImmediate(const shil_opcode& op)
	{
		if (!op.rs1.is_imm())
			return false;
		void *ptr;
		bool isram;
		u32 addr;
		if (!rdv_readMemImmediate(op.rs1._imm, op.size, ptr, isram, addr, block))
			return false;

		if (isram)
		{
			Ldr(x1, reinterpret_cast<uintptr_t>(ptr));	// faster than Mov
			if (regalloc.IsAllocAny(op.rd))
			{
				switch (op.size)
				{
				case 1:
					Ldrsb(regalloc.MapRegister(op.rd), MemOperand(x1));
					break;

				case 2:
					Ldrsh(regalloc.MapRegister(op.rd), MemOperand(x1));
					break;

				case 4:
					if (op.rd.is_r32f())
						Ldr(regalloc.MapVRegister(op.rd), MemOperand(x1));
					else
						Ldr(regalloc.MapRegister(op.rd), MemOperand(x1));
					break;

				case 8:
					Ldp(regalloc.MapVRegister(op.rd, 0), regalloc.MapVRegister(op.rd, 1), MemOperand(x1));
					break;

				default:
					die("Invalid size");
					break;
				}
			}
			else
			{
				switch (op.size)
				{
				case 1:
					Ldrsb(w1, MemOperand(x1));
					break;

				case 2:
					Ldrsh(w1, MemOperand(x1));
					break;

				case 4:
					Ldr(w1, MemOperand(x1));
					break;

				case 8:
					Ldr(x1, MemOperand(x1));
					break;

				default:
					die("Invalid size");
					break;
				}
				if (op.size == 8)
					Str(x1, sh4_context_mem_operand(op.rd._reg));
				else
					Str(w1, sh4_context_mem_operand(op.rd._reg));
			}
		}
		else
		{
			// Not RAM
			if (op.size == 8)
			{
				// Need to call the handler twice
				Mov(w0, addr);
				GenCallRuntime((void (*)())ptr);
				if (regalloc.IsAllocf(op.rd))
					Fmov(regalloc.MapVRegister(op.rd, 0), w0);
				else
					Str(w0, sh4_context_mem_operand(op.rd._reg));

				Mov(w0, addr + 4);
				GenCallRuntime((void (*)())ptr);
				if (regalloc.IsAllocf(op.rd))
					Fmov(regalloc.MapVRegister(op.rd, 1), w0);
				else
					Str(w0, sh4_context_mem_operand((Sh4RegType)(op.rd._reg + 1)));
			}
			else
			{
				Mov(w0, addr);

				switch(op.size)
				{
				case 1:
					GenCallRuntime((void (*)())ptr);
					Sxtb(w0, w0);
					break;

				case 2:
					GenCallRuntime((void (*)())ptr);
					Sxth(w0, w0);
					break;

				case 4:
					GenCallRuntime((void (*)())ptr);
					break;

				default:
					die("Invalid size");
					break;
				}

				if (regalloc.IsAllocg(op.rd))
					Mov(regalloc.MapRegister(op.rd), w0);
				else
				{
					verify(regalloc.IsAllocf(op.rd));
					Fmov(regalloc.MapVRegister(op.rd), w0);
				}
			}
		}

		return true;
	}

	bool GenReadMemoryFast(const shil_opcode& op, size_t opid)
	{
		// Direct memory access. Need to handle SIGSEGV and rewrite block as needed. See rewrite()
		if (!addrspace::virtmemEnabled())
			return false;

		Instruction *start_instruction = GetCursorAddress<Instruction *>();

		// WARNING: the rewrite code relies on having 2 ops before the memory access
		// Update rewrite (and perhaps read_memory_rewrite_size) if adding or removing code
		Ubfx(x1, x0, 0, 29);
		Add(x1, x1, sizeof(Sh4Context), LeaveFlags);

		switch (op.size)
		{
		case 1:
			Ldrsb(w0, MemOperand(x28, x1));
			break;

		case 2:
			Ldrsh(w0, MemOperand(x28, x1));
			break;

		case 4:
			Ldr(w0, MemOperand(x28, x1));
			break;

		case 8:
			Ldr(x0, MemOperand(x28, x1));
			break;
		}
		EnsureCodeSize(start_instruction, read_memory_rewrite_size);

		return true;
	}

	void GenWriteMemory(const shil_opcode& op, size_t opid, bool optimise)
	{
		if (GenWriteMemoryImmediate(op, optimise))
			return;

		if (compactMemAccess(op, optimise))
		{
			// Deadcast: as GenReadMemory, stored straight from the register the value is in
			const Register& raddr = GenMemAddr(op);
			MmuAccess *mmu = windowAccesses() ? &genMmuAccess(op, 1, raddr) : nullptr;
			CPURegister data = w1;
			if (op.size == 8)
			{
				shil_param_to_host_reg(op.rs2, x1);
				data = x1;
			}
			else if (regalloc.IsAllocg(op.rs2))
				data = regalloc.MapRegister(op.rs2);
			else if (op.size == 4 && regalloc.IsAllocf(op.rs2))
				data = regalloc.MapVRegister(op.rs2);
			else
				shil_param_to_host_reg(op.rs2, w1);
			if (mmu != nullptr && op.size >= 4 && !mmu_enabled() && sqStoreSites.count(block->vaddr + op.guest_offs) != 0)
			{
				// Deadcast: a store that wrote to the store queues: there inline (sq_buffer, at the context's start),
				// anything else through its stub (which looks again)
				Lsr(w3, raddr, 26);
				Cmp(w3, 0x38);
				B(&mmu->stub, ne);
				And(x3, raddr.X(), 0x3f);
				mmu->accessOffset = (u32)GetBuffer()->GetCursorOffset();
				if (op.size == 8)
					Str(x1, MemOperand(x28, x3));
				else if (data.IsVRegister())
					Str(data.S(), MemOperand(x28, x3));
				else
					Str(data.W(), MemOperand(x28, x3));
				Bind(&mmu->next);
				mmu->size = op.size;
				mmu->data = data;
				return;
			}
			if (mmu != nullptr)
			{
				const MemOperand addr(x27, raddr, UXTW);
				mmu->accessOffset = (u32)GetBuffer()->GetCursorOffset();
				switch (op.size)
				{
				case 1:
					Strb(data.W(), addr);
					break;
				case 2:
					Strh(data.W(), addr);
					break;
				case 4:
					if (data.IsVRegister())
						Str(data.S(), addr);
					else
						Str(data.W(), addr);
					break;
				case 8:
					Str(x1, addr);
					break;
				}
				Bind(&mmu->next);
				mmu->size = op.size;
				mmu->data = data;
				return;
			}
			Instruction *start_instruction = GetCursorAddress<Instruction *>();
			Ubfx(x7, raddr.X(), 0, 29);
			switch (op.size)
			{
			case 1:
				Strb(data.W(), MemOperand(x27, x7));
				break;
			case 2:
				Strh(data.W(), MemOperand(x27, x7));
				break;
			case 4:
				if (data.IsVRegister())
					Str(data.S(), MemOperand(x27, x7));
				else
					Str(data.W(), MemOperand(x27, x7));
				break;
			case 8:
				Str(x1, MemOperand(x27, x7));
				break;
			}
			EnsureCodeSize(start_instruction, compact_write_size);
			return;
		}

		GenMemAddr(op, &w0);
		genMmuLookup(op, 1);

		if (op.size != 8)
			shil_param_to_host_reg(op.rs2, w1);
		else
			shil_param_to_host_reg(op.rs2, x1);
		if (optimise && GenWriteMemoryFast(op, opid))
			return;

		GenWriteMemorySlow(op.size);
		if (mmu_enabled())
			// Deadcast: it may have written PTEH, changing the ASID and its window
			genReloadWindow();
	}

	bool GenWriteMemoryImmediate(const shil_opcode& op, bool optimise)
	{
		if (!op.rs1.is_imm())
			return false;

		void *ptr;
		bool isram;
		u32 addr;
		if (!rdv_writeMemImmediate(op.rs1._imm, op.size, ptr, isram, addr, block))
			return false;
		// Deadcast: to RAM, through the window as any store, with its stub: one that writes next to protected code
		// can go to it (bm_StoreToSlowPath), a store to a pointer cannot
		if (isram && (addr & 0x1C000000) == 0x0C000000 && !mmu_enabled() && windowAccesses()
				&& compactMemAccess(op, optimise))
			return false;

		if (isram)
		{
			Register reg2;
			if (op.rs2.is_imm())
			{
				Mov(w1, op.rs2._imm);
				reg2 = w1;
			}
			else if (regalloc.IsAllocg(op.rs2))
			{
				reg2 = regalloc.MapRegister(op.rs2);
			}
			else if (op.size == 8)
			{
				shil_param_to_host_reg(op.rs2, x1);
				reg2 = x1;
			}
			else if (regalloc.IsAllocf(op.rs2))
			{
				Fmov(w1, regalloc.MapVRegister(op.rs2));
				reg2 = w1;
			}
			else
				die("Invalid rs2 param");

			Ldr(x0, reinterpret_cast<uintptr_t>(ptr));
			switch (op.size)
			{
			case 1:
				Strb(reg2, MemOperand(x0));
				break;

			case 2:
				Strh(reg2, MemOperand(x0));
				break;

			case 4:
				Str(reg2, MemOperand(x0));
				break;

			case 8:
				Str(reg2, MemOperand(x0));
				break;

			default:
				die("Invalid size");
				break;
			}
		}
		else
		{
			// Not RAM
			Mov(w0, addr);
			shil_param_to_host_reg(op.rs2, x1);
			if (op.size == 8)
			{
				// Need to call the handler twice
				GenCallRuntime((void (*)())ptr);

				Mov(w0, addr + 4);
				shil_param_to_host_reg(op.rs2, x1);
				Lsr(x1, x1, 32);
				GenCallRuntime((void (*)())ptr);
			}
			else
			{
				if (op.size == 1)
					Uxtb(w1, w1);
				else if (op.size == 2)
					Uxth(w1, w1);
				GenCallRuntime((void (*)())ptr);
			}
			if (mmu_enabled())
				// Deadcast: it may have written PTEH, changing the ASID and its window
				genReloadWindow();
		}

		return true;
	}

	bool GenWriteMemoryFast(const shil_opcode& op, size_t opid)
	{
		// Direct memory access. Need to handle SIGSEGV and rewrite block as needed. See rewrite()
		if (!addrspace::virtmemEnabled())
			return false;

		Instruction *start_instruction = GetCursorAddress<Instruction *>();

		// WARNING: the rewrite code relies on having 2 ops before the memory access
		// Update rewrite (and perhaps write_memory_rewrite_size) if adding or removing code
		Ubfx(x7, x0, 0, 29);
		Add(x7, x7, sizeof(Sh4Context), LeaveFlags);

		switch(op.size)
		{
		case 1:
			Strb(w1, MemOperand(x28, x7));
			break;

		case 2:
			Strh(w1, MemOperand(x28, x7));
			break;

		case 4:
			Str(w1, MemOperand(x28, x7));
			break;

		case 8:
			Str(x1, MemOperand(x28, x7));
			break;
		}
		EnsureCodeSize(start_instruction, write_memory_rewrite_size);

		return true;
	}

	void EnsureCodeSize(Instruction *start_instruction, int code_size)
	{
		while (GetCursorAddress<Instruction *>() - start_instruction < code_size * kInstructionSize)
			Nop();
		verify (GetCursorAddress<Instruction *>() - start_instruction == code_size * kInstructionSize);
	}

	void CheckBlock(bool force_checks, RuntimeBlockInfo* block)
	{
		if (mmu_enabled())
		{
			// Verify that pc == block->vaddr
			// Deadcast: inline, the failure out of line (genBlockChecksFailed); a linked exit to this
			// vaddr enters past it (GenMmuStaticExit)
			Ldr(w2, sh4_context_mem_operand(&sh4ctx.pc));
			Mov(w0, block->vaddr);
			Cmp(w2, w0);
			B(&pcCheckFailed, ne);
			block->pcCheckSize = (u32)GetBuffer()->GetCursorOffset();
		}

		if (force_checks)
		{
			// Verify that the block hasn't been modified
			Label blockcheck_fail;
			s32 sz = block->sh4_code_size;
			u8* ptr = GetMemPtr(block->addr, sz);
			if (ptr != NULL)
			{
				Ldr(x1, reinterpret_cast<uintptr_t>(ptr));

				while (sz > 0)
				{
					if (sz >= 8)
					{
						Ldr(x2, MemOperand(x1, 8, PostIndex));
						Ldr(x3, *(u64*)ptr);
						Cmp(x2, x3);
						sz -= 8;
						ptr += 8;
					}
					else if (sz >= 4)
					{
						Ldr(w2, MemOperand(x1, 4, PostIndex));
						Ldr(w3, *(u32*)ptr);
						Cmp(w2, w3);
						sz -= 4;
						ptr += 4;
					}
					else
					{
						Ldrh(w2, MemOperand(x1, 2, PostIndex));
						Mov(w3, *(u16*)ptr);
						Cmp(w2, w3);
						sz -= 2;
						ptr += 2;
					}
					B(ne, &blockcheck_fail);
				}
			}

			Label blockcheck_success;
			B(&blockcheck_success);
			Bind(&blockcheck_fail);
			Mov(w0, block->addr);
			GenBranch(blockCheckFail);

			Bind(&blockcheck_success);
		}

		if (mmu_enabled() && block->has_fpu_op)
		{
			// Verify that the FPU is enabled
			// Deadcast: inline, the exception out of line (genBlockChecksFailed)
			Ldr(w3, sh4_context_mem_operand(&sh4ctx.sr.status));
			Tbnz(w3, 15, &fpuDisabled);	// SR.FD
		}
	}

	// Deadcast: where CheckBlock's checks go when they fail, at the end of the block, gone around
	void genBlockChecksFailed()
	{
		if (!pcCheckFailed.IsLinked() && !fpuDisabled.IsLinked())
			return;
		Label done;
		B(&done);
		if (pcCheckFailed.IsLinked())
		{
			Bind(&pcCheckFailed);
			Mov(w0, block->addr);
			GenBranch(blockCheckFail);
		}
		if (fpuDisabled.IsLinked())
		{
			Bind(&fpuDisabled);
			// raises the exception, as SR.FD is set
			Mov(w0, block->vaddr);
			GenBranch(checkBlockFpu);
		}
		Bind(&done);
	}

	void shil_param_to_host_reg(const shil_param& param, const Register& reg)
	{
		if (param.is_imm())
		{
			Mov(reg, param._imm);
		}
		else if (param.is_reg())
		{
			if (param.is_r64f() && !regalloc.IsAllocf(param))
			{
				Ldr(reg, sh4_context_mem_operand(param._reg));
			}
			else if (param.is_r32f() || param.is_r64f())
			{
				if (regalloc.IsAllocf(param))
					Fmov(reg.W(), regalloc.MapVRegister(param, 0));
				else
					Ldr(reg.W(), sh4_context_mem_operand(param._reg));
				if (param.is_r64f())
				{
					Fmov(w17, regalloc.MapVRegister(param, 1));
					Bfm(reg, x17, 32, 31);
				}
			}
			else
			{
				if (regalloc.IsAllocg(param))
					Mov(reg.W(), regalloc.MapRegister(param));
				else
					Ldr(reg.W(), sh4_context_mem_operand(param._reg));
			}
		}
		else
		{
			verify(param.is_null());
		}
	}

	void host_reg_to_shil_param(const shil_param& param, const CPURegister& reg)
	{
		if (reg.Is64Bits())
		{
			if (regalloc.IsAllocf(param))
			{
				verify(param.count() == 2);
				Fmov(regalloc.MapVRegister(param, 0), reg.W());
				Lsr(reg.X(), reg.X(), 32);
				Fmov(regalloc.MapVRegister(param, 1), reg.W());
			}
			else
			{
				Str((const Register&)reg, sh4_context_mem_operand(param._reg));
			}
		}
		else if (regalloc.IsAllocg(param))
		{
			if (reg.IsRegister())
				Mov(regalloc.MapRegister(param), (const Register&)reg);
			else
				Fmov(regalloc.MapRegister(param), (const VRegister&)reg);
		}
		else if (regalloc.IsAllocf(param))
		{
			if (reg.IsVRegister())
				Fmov(regalloc.MapVRegister(param), (const VRegister&)reg);
			else
				Fmov(regalloc.MapVRegister(param), (const Register&)reg);
		}
		else
		{
			Str(reg, sh4_context_mem_operand(param._reg));
		}
	}

	struct CC_PS
	{
		CanonicalParamType type;
		const shil_param *prm;
	};
	std::vector<CC_PS> CC_pars;
	std::vector<const WRegister*> call_regs;
	std::vector<const XRegister*> call_regs64;
	std::vector<const VRegister*> call_fregs;
	Arm64RegAlloc regalloc;
	RuntimeBlockInfo* block = NULL;
	Label pcCheckFailed;	// CheckBlock's, out of line (genBlockChecksFailed)
	Label fpuDisabled;
	// Deadcast: while the main loop is generated, its thunks, generated at its end (GenMainloop)
	struct PendingThunk
	{
		uintptr_t function = 0;
		Label label;
	};
	std::list<PendingThunk> *pendingThunks = nullptr;
	Label *pendingCallThunk = nullptr;
	bool inThunk = false;
	const int read_memory_rewrite_size = 5;	// ubfx, add, ldr for fast access. calling a handler can use more than 3 depending on offset
	const int write_memory_rewrite_size = 5; // ubfx, add, str
	// Deadcast: the compact accesses (compactMemAccess): ubfx, add, load; ubfx, add, store, nop
	static constexpr int compact_read_size = 3;
	static constexpr int compact_write_size = 4;
	Sh4Context& sh4ctx;
	Sh4CodeBuffer& codeBuffer;
};

class Arm64Dynarec : public Sh4Dynarec
{
public:
	Arm64Dynarec() {
		sh4Dynarec = this;
	}

	void init(Sh4Context& sh4ctx, Sh4CodeBuffer& codeBuffer) override
	{
		INFO_LOG(DYNAREC, "Initializing the ARM64 dynarec");
		this->sh4ctx = &sh4ctx;
		this->codeBuffer = &codeBuffer;
	}

	void reset() override
	{
		unwinder.clear();
		::mainloop = nullptr;

		if (sh4ctx->CpuRunning)
		{
			// Force the dynarec out of mainloop() to regenerate it
			sh4ctx->CpuRunning = 0;
			restarting = true;
		}
		else
			generate_mainloop();
	}

	void mainloop(void* v_cntx) override
	{
		try {
			do {
				restarting = false;
				generate_mainloop();
				sqTaRegionsUpdate();

				::mainloop(v_cntx);
				if (restarting && !emu.restartCpu())
					restarting = false;
			} while (restarting);
		} catch (const SH4ThrownException&) {
			ERROR_LOG(DYNAREC, "SH4ThrownException in mainloop");
			throw FlycastException("Fatal: Unhandled SH4 exception");
		}
	}

	void compile(RuntimeBlockInfo* block, bool smc_checks, bool optimise) override
	{
		verify(codeBuffer->getFreeSpace() >= 16 * 1024);

		compiler = new Arm64Assembler(*sh4ctx, *codeBuffer);

		compiler->compileBlock(block, smc_checks, optimise);

		delete compiler;
		compiler = nullptr;
	}

	void canonStart(const shil_opcode *op) override
	{
		compiler->canonStart(op);
	}

	void canonParam(const shil_opcode *op, const shil_param *par, CanonicalParamType tp) override
	{
		compiler->canonParam(*op, par, tp);
	}

	void canonCall(const shil_opcode *op, void *function) override
	{
		compiler->canonCall(op, function);
	}

	void canonFinish(const shil_opcode *op) override {
	}

	void generate_mainloop()
	{
		if (::mainloop != nullptr)
			return;
		jitWriteProtect(*codeBuffer, false);
		compiler = new Arm64Assembler(*sh4ctx, *codeBuffer);

		compiler->GenMainloop();

		delete compiler;
		compiler = nullptr;
		jitWriteProtect(*codeBuffer, true);
	}

	RuntimeBlockInfo* allocateBlock() override
	{
		generate_mainloop();
		return new DynaRBI(*sh4ctx, *codeBuffer);
	}

	void handleException(host_context_t &context) override
	{
		context.pc = (uintptr_t)::handleException;
	}

	bool storeToStub(host_context_t &context, void *faultAddress) override
	{
		if (codeBuffer == nullptr || mmu_enabled() || addrspace::windowToMap(faultAddress) == nullptr)
			return false;
		u32 *code_ptr = (u32 *)CC_RX2RW(context.pc);
		if ((u8 *)code_ptr < (u8 *)codeBuffer->getBase() || (u8 *)code_ptr >= (u8 *)codeBuffer->getBase() + codeBuffer->getSize())
			return false;
		// str/strb/strh/str s [x27, wN, uxtw] (genMmuAccess's)
		const u32 op = *code_ptr;
		if ((op & 0x3B200C00) != 0x38200800 || ((op >> 13) & 7) != 2 || ((op >> 5) & 31) != 27 || ((op >> 22) & 3) != 0)
			return false;
		RuntimeBlockInfoPtr block = bm_GetBlock((void *)context.pc);
		if (block == nullptr)
			return false;
		const u32 offset = (u32)((u8 *)code_ptr - (u8 *)block->code);
		u32 *stub = nullptr;
		for (const auto& [access, stubOffset] : block->mmuStubs)
			if (access == offset)
				stub = (u32 *)((u8 *)block->code + stubOffset);
		if (stub == nullptr)
			return false;
		jitWriteProtect(*codeBuffer, false);
		*code_ptr = 0x14000000 | ((u32)(stub - code_ptr) & 0x3FFFFFF);
		virtmem::flush_cache(CC_RW2RX(code_ptr), CC_RW2RX(code_ptr + 1), code_ptr, code_ptr + 1);
		context.pc = (uintptr_t)CC_RW2RX(stub);
		jitWriteProtect(*codeBuffer, true);
		return true;
	}

	bool rewrite(host_context_t &context, void *faultAddress) override
	{
		constexpr u32 STR_LDR_MASK = 0xFFE0EC00;

		static const u32 armv8_mem_ops[] = {
				0x38E06800,		// Ldrsb
				0x78E06800,		// Ldrsh
				0xB8606800,		// Ldr w
				0xF8606800,		// Ldr x
				0x38206800,		// Strb
				0x78206800,		// Strh
				0xB8206800,		// Str w
				0xF8206800,		// Str x
				0xBC606800,		// Ldr s (Deadcast: compact accesses load and store float registers)
				0xBC206800,		// Str s
		};
		static const bool read_ops[] = {
				true,
				true,
				true,
				true,
				false,
				false,
				false,
				false,
				true,
				false,
		};
		static const u32 op_sizes[] = {
				1,
				2,
				4,
				8,
				1,
				2,
				4,
				8,
				4,
				4,
		};

		if (codeBuffer == nullptr)
			// init() not called yet
			return false;
		//LOGI("Sh4Dynarec::rewrite pc %zx\n", context.pc);
		u32 *code_ptr = (u32 *)CC_RX2RW(context.pc);
		if ((u8 *)code_ptr < (u8 *)codeBuffer->getBase()
				|| (u8 *)code_ptr >= (u8 *)codeBuffer->getBase() + codeBuffer->getSize())
			return false;
		jitWriteProtect(*codeBuffer, false);
		u32 armv8_op = *code_ptr;
		// Deadcast: under the MMU, an access through the window, ldr/str [x27, wN, uxtw] (genMmuAccess):
		// RAM is mapped there, a store to a page whose code is protected unprotects it, and anything
		// else goes to the access's stub, for good if it is not RAM (the patched access branches
		// there), this once if translating raises an exception (the stub then raises it).
		u32 vaddr;
		if ((armv8_op & 0x3B200C00) == 0x38200800 && ((armv8_op >> 13) & 7) == 2 && ((armv8_op >> 5) & 31) == 27
				&& !mmu_enabled() && addrspace::windowToMap(faultAddress) != nullptr)
		{
			// Deadcast: without the MMU, an access through the dynarec's window (addrspace.h) to what it does
			// not map (a write to protected RAM or VRAM is let through by the fault handler first):
			// its stub, for good
			RuntimeBlockInfoPtr block = bm_GetBlock((void *)context.pc);
			// a block thrown away to be compiled again (below) still runs to its end
			const bool live = block != nullptr;
			if (!live)
				block = bm_GetStaleBlock((void *)context.pc);
			verify(block != nullptr);
			const u32 offset = (u32)((u8 *)code_ptr - (u8 *)block->code);
			u32 *stub = nullptr;
			u32 site = 0;
			for (size_t i = 0; i < block->mmuStubs.size(); i++)
				if (block->mmuStubs[i].first == offset)
				{
					stub = (u32 *)((u8 *)block->code + block->mmuStubs[i].second);
					site = block->mmuSites[i];
				}
			verify(stub != nullptr);
			*code_ptr = 0x14000000 | ((u32)(stub - code_ptr) & 0x3FFFFFF);
			virtmem::flush_cache(CC_RW2RX(code_ptr), CC_RW2RX(code_ptr + 1), code_ptr, code_ptr + 1);
			context.pc = (uintptr_t)CC_RW2RX(stub);
			jitWriteProtect(*codeBuffer, true);
			// Deadcast: a store to the store queues (the TA's vertices, for every game): its block compiled again with it
			// inline (GenWriteMemory, sqStoreSites)
			const u32 guest = (u32)((u8 *)faultAddress - addrspace::window);
			if (((armv8_op >> 22) & 3) == 0 && (guest >> 26) == 0x38 && sqStoreSites.insert(site).second && live)
				bm_DiscardBlock(block.get());
			return true;
		}
		if ((armv8_op & 0x3B200C00) == 0x38200800 && ((armv8_op >> 13) & 7) == 2 && ((armv8_op >> 5) & 31) == 27
				&& mmu_enabled() && mmuwin::inWindow(faultAddress, vaddr))
		{
			const bool write = ((armv8_op >> 22) & 3) == 0;
			u32 paddr;
			const MmuError err = write ? mmu_data_translation<MMU_TT_DWRITE>(vaddr, paddr)
					: mmu_data_translation<MMU_TT_DREAD>(vaddr, paddr);
			if (err == MmuError::NONE && isRam(paddr))
			{
				if (!mmuwin::isMapped(vaddr))
					mmuwin::mapPage(vaddr, paddr);
				else if (write)
					bm_RamWriteAccess(paddr & RAM_MASK);
				jitWriteProtect(*codeBuffer, true);
				return true;
			}
			RuntimeBlockInfoPtr block = bm_GetBlock((void *)context.pc);
			verify(block != nullptr);
			const u32 offset = (u32)((u8 *)code_ptr - (u8 *)block->code);
			u32 *stub = nullptr;
			for (const auto& [access, stubOffset] : block->mmuStubs)
				if (access == offset)
					stub = (u32 *)((u8 *)block->code + stubOffset);
			verify(stub != nullptr);
			if (err == MmuError::NONE)
			{
				*code_ptr = 0x14000000 | ((u32)(stub - code_ptr) & 0x3FFFFFF);
				virtmem::flush_cache(CC_RW2RX(code_ptr), CC_RW2RX(code_ptr + 1), code_ptr, code_ptr + 1);
			}
			context.pc = (uintptr_t)CC_RW2RX(stub);
			jitWriteProtect(*codeBuffer, true);
			return true;
		}
		bool is_read = false;
		bool is_fp = false;
		u32 size = 0;
		bool found = false;
		u32 masked = armv8_op & STR_LDR_MASK;
		for (u32 i = 0; i < std::size(armv8_mem_ops); i++)
		{
			if (masked == armv8_mem_ops[i])
			{
				size = op_sizes[i];
				is_read = read_ops[i];
				is_fp = i >= 8;
				found = true;
				break;
			}
		}
		verify(found);

		// Skip the preceding ops (add, ubfx)
		// (the compact accesses without the MMU have no add: the ubfx is right before the access)
		const bool ubfxBefore = (code_ptr[-1] & 0xFFC00000) == 0xD3400000;
		u32 *code_rewrite = ubfxBefore ? code_ptr - 1 : code_ptr - 2;
		// Deadcast: the address is in the register the ubfx takes it from, the data in the load's or
		// store's (GenReadMemory, GenWriteMemory): the old accesses have them in w0 and w0/w1
		const u32 addrReg = (code_rewrite[0] >> 5) & 31;
		const u32 dataReg = armv8_op & 31;
		const bool storeQueue = (context.x[addrReg] >> 26) == 0x38;
		Arm64Assembler *assembler = new Arm64Assembler(*sh4ctx, *codeBuffer, code_rewrite);
		assembler->GenMemoryRewrite(is_read, size, is_fp, addrReg, dataReg, storeQueue);
		assembler->Finalize(true);
		delete assembler;
		context.pc = (uintptr_t)CC_RW2RX(code_rewrite);
		jitWriteProtect(*codeBuffer, true);

		return true;
	}

private:
	Arm64Assembler* compiler = nullptr;
	bool restarting = false;
	Sh4Context *sh4ctx = nullptr;
	Sh4CodeBuffer *codeBuffer = nullptr;
};

static Arm64Dynarec instance;

u32 DynaRBI::Relink()
{
#ifndef NO_BLOCK_LINKING
	//printf("DynaRBI::Relink %08x\n", this->addr);
	jitWriteProtect(codeBuffer, false);
	Arm64Assembler *compiler = new Arm64Assembler(sh4ctx, codeBuffer, (u8 *)this->code + this->relink_offset);

	u32 code_size = compiler->RelinkBlock(this);
	compiler->Finalize(true);
	delete compiler;
	jitWriteProtect(codeBuffer, true);

	return code_size;
#else
	return 0;
#endif
}

void Arm64RegAlloc::Preload(u32 reg, eReg nreg)
{
	assembler->Ldr(Register(nreg, 32), assembler->sh4_context_mem_operand((Sh4RegType)reg));
}
void Arm64RegAlloc::Writeback(u32 reg, eReg nreg)
{
	assembler->Str(Register(nreg, 32), assembler->sh4_context_mem_operand((Sh4RegType)reg));
}
void Arm64RegAlloc::Preload_FPU(u32 reg, eFReg nreg)
{
	assembler->Ldr(VRegister(nreg, 32), assembler->sh4_context_mem_operand((Sh4RegType)reg));
}
void Arm64RegAlloc::Writeback_FPU(u32 reg, eFReg nreg)
{
	assembler->Str(VRegister(nreg, 32), assembler->sh4_context_mem_operand((Sh4RegType)reg));
}
#endif	// FEAT_SHREC == DYNAREC_JIT
