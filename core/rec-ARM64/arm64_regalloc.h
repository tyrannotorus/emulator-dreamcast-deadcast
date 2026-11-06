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
#pragma once
#include "hw/sh4/dyna/ssa_regalloc.h"
#include <aarch64/macro-assembler-aarch64.h>
using namespace vixl::aarch64;

#define ALLOC_F64 true

enum eReg {
	W0, W1, W2, W3, W4, W5, W6, W7, W8, W9, W10, W11, W12, W13, W14, W15, W16,
	W17, W18, W19, W20, W21, W22, W23, W24, W25, W26, W27, W28, W29, W30
};
enum eFReg {
	S0, S1, S2, S3, S4, S5, S6, S7, S8, S9, S10, S11, S12, S13, S14, S15, S16,
	S17, S18, S19, S20, S21, S22, S23, S24, S25, S26, S27, S28, S29, S30, S31
};

static eReg alloc_regs[] = { W19, W20, W21, W22, W23, W24, W25, W26, (eReg)-1 };
static eFReg alloc_fregs[] = { S8, S9, S10, S11, S12, S13, S14, S15, (eFReg)-1 };

// Deadcast: without the MMU, R0-R15 and T live in host registers for good (pinned, RegAlloc::Pin), so
// that blocks neither load them at their start nor store them at their end: R0-R7 in w19-w26, R8-R15
// in w8-w15, T in w18. What else blocks use goes in w4-w7. Calls out of translated code keep all of
// these (its thunks, rec_arm64.cpp), and only w0-w3, w16 and w17 are scratch.
static constexpr eReg pinned_gregs[16] = { W19, W20, W21, W22, W23, W24, W25, W26, W8, W9, W10, W11, W12, W13, W14, W15 };
static constexpr eReg pinned_T = W18;
static eReg alloc_regs_pinned[] = { W4, W5, W6, W7, (eReg)-1 };

class Arm64Assembler;

struct Arm64RegAlloc : RegAlloc<eReg, eFReg, ALLOC_F64>
{
	Arm64RegAlloc(Arm64Assembler *assembler) : assembler(assembler) {}

	void DoAlloc(RuntimeBlockInfo* block, bool pinning)
	{
		// Deadcast: off. The lazy write-back under the MMU (registers written back only where an access's translation misses,
		// Arm64Assembler::genMmuSlowLookups) wrote stale host registers over the context: Armada's game, past its intro, never ran.
		lazyMmuWriteback = false;
		jdynInHostReg = true;		// Deadcast: the exits use w29 (RelinkBlock)
		Unpin();
		if (pinning)
		{
			for (int i = 0; i < 16; i++)
				Pin((Sh4RegType)(reg_r0 + i), pinned_gregs[i]);
			Pin(reg_sr_T, pinned_T);
			RegAlloc::DoAlloc(block, alloc_regs_pinned, alloc_fregs);
		}
		else
			RegAlloc::DoAlloc(block, alloc_regs, alloc_fregs);
	}

	void Preload(u32 reg, eReg nreg) override;
	void Writeback(u32 reg, eReg nreg) override;
	void Preload_FPU(u32 reg, eFReg nreg) override;
	void Writeback_FPU(u32 reg, eFReg nreg) override;

	const Register& MapRegister(const shil_param& param)
	{
		#define WREG(n) vixl::aarch64::w##n,
		static const Register wregisters[] = {AARCH64_REGISTER_CODE_LIST(WREG)};
		#undef WREG

		eReg ereg = mapg(param);
		if (ereg == (eReg)-1)
			die("Register not allocated");
		return wregisters[ereg];
	}

	const VRegister& MapVRegister(const shil_param& param, int index = 0)
	{
		#define SREG(n) vixl::aarch64::s##n,
		static const VRegister sregisters[] = {AARCH64_REGISTER_CODE_LIST(SREG)};
		#undef SREG

		eFReg ereg = mapf(param, index);
		if (ereg == (eFReg)-1)
			die("VRegister not allocated");
		return sregisters[ereg];
	}

	Arm64Assembler *assembler;
};
