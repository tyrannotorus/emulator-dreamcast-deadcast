// Deadcast: the 2K engine's TA submit loop (NFL 2K2 0c03d084-0c03d0bc, in the mesh routine 0c03cda0-0c03d0c6), in
// hand-written ARM64. Shared, as is, by the emulator (hotpaths.cpp) and its harness (local/brick/submitbench), which
// proves it against the C++ transcription byte for byte before it runs here.
//
// The SH4 loop, a strip's vertices from the on-chip RAM (R10, from the stream at R8) through the store queues (R0) to
// the TA (pref), the strip ended by 0xf0000000 (its literal at d0d8):
//   d084  mov.l @R8+,R2 ; tst R2,R2 ; bt.s d0c0 ; nop                       the strip's count; 0: the strips' end
//   d08c  mov.l @R8+,R10 ; dt R2 ; R11-R13 <- @(12..32,R10) to @(4..28,R0) ; bt.s d0b4 ; mov.l R4,@R0
//   d0ac  pref @R0 ; bra d08c ; add #32,R0
//   d0b4  mov.l @(d0d8),R13 ; mov.l R13,@R0 ; pref @R0 ; bra d084 ; add #32,R0
// Each label a block of the dynarec's: charged its cycles at its start. Where they have run out, the next timeslice is
// taken as the dynarec's arm64_intc_sched takes it, when no event is due and no interrupt pending (the timeslice added
// to the cycles, taken off sh4_sched_next); else it stops there, for the dynarec's block to call the scheduler. It also
// stops, untouched, at a block's start where its fast path does not apply (the stream not in main RAM, the vertex not
// in the on-chip RAM or across its 4 KB, the store queue address not one), for the dynarec's block to run it.
// pref is the dynarec's own store queue flush (rec_arm64.cpp, sqFlush): to the TA's polygon FIFO, its state machine
// stepped, its handler called for a transition; anything else through doSqWrite.
//
// Registers: x19 the SubmitIo, w20 R0, w21 R2, w22 R4, w23 R8, w24 R10, w25 R11, w26 R12, w27 R13, w28 the cycles;
// x0-x17 scratch. Called as a C function (through the dynarec's call thunk, which keeps its pinned registers).
#pragma once
#include <cstddef>
#include <cstdint>

namespace deadcast
{

enum SubmitBlock : uint32_t { SubmitD084, SubmitD08c, SubmitD0ac, SubmitD0b4, SubmitD0c0 };

struct SubmitIo
{
	uint32_t r0, r2, r4, r8;			// 0
	uint32_t r10, r11, r12, r13;		// 16
	int32_t cycles;						// 32
	uint32_t entry;						// 36: the block it starts in (its cycles charged), SubmitBlock
	int32_t cost[4];					// 40: the blocks' cycles, d084 d08c d0ac d0b4
	uint32_t endOfStrip;				// 56: the literal at d0d8
	uint32_t ramMask;					// 60
	uint8_t *ram;						// 64: main RAM
	uint8_t *ocr;						// 72: the on-chip RAM (only with ORA)
	uint32_t oixShift;					// 80: 13 with OIX, else 1 (onChipRamOffset)
	uint32_t ora;						// 84
	uint8_t *sq;						// 88: the store queues, 2 x 32 bytes
	const uint8_t *taRegions;			// 96: a store queue area's 1 MB to the TA's polygon FIFO (sqTaRegionsUpdate)
	void **taContext;					// 104
	uint8_t **taDataWrite;				// 112
	intptr_t taDataRootOffset;			// 120
	uintptr_t taDataSize;				// 128
	uint8_t *taFsm;						// 136
	void (*taHandle)(uint32_t);			// 144
	void (*sqWrite)(uint32_t, void *);	// 152
	void *sqWriteCtx;					// 160
	int32_t *schedNext;					// 168: Sh4Context::sh4_sched_next
	uint32_t *interruptPend;			// 176: Sh4Context::interrupt_pend
	uint32_t *cpuRunning;				// 184: Sh4Context::CpuRunning
};
static_assert(offsetof(SubmitIo, cycles) == 32 && offsetof(SubmitIo, cost) == 40 && offsetof(SubmitIo, ram) == 64
		&& offsetof(SubmitIo, sq) == 88 && offsetof(SubmitIo, taFsm) == 136 && offsetof(SubmitIo, sqWriteCtx) == 160
		&& offsetof(SubmitIo, cpuRunning) == 184,
		"SubmitIo's layout is the assembly's");

// returns the block it stopped at (SubmitBlock), | SubmitBailed where its fast path did not apply
constexpr uint32_t SubmitBailed = 0x100;
extern "C" uint32_t deadcast_submit(SubmitIo *io);

}

asm(R"(
	// x19 the SubmitIo; w20 R0, w21 R2, w22 R4, w23 R8, w24 R10, w25 R11, w26 R12, w27 R13, w28 the cycles (kept across
	// calls); x8 RAM, w9 its mask, x10 the on-chip RAM (0 without ORA), w11 the OIX shift, x12 the store queues, x13 the TA
	// regions, x14 &taDataWrite, x15 ta_fsm, x16 the TA data's root, x17 its end less 64 (0 without a TA context): taken
	// again after any call (.Lreload); x0-x7 scratch.
	.text
	.p2align 4
	.global deadcast_submit
	.type deadcast_submit, %function
deadcast_submit:
	stp	x29, x30, [sp, #-96]!
	mov	x29, sp
	stp	x19, x20, [sp, #16]
	stp	x21, x22, [sp, #32]
	stp	x23, x24, [sp, #48]
	stp	x25, x26, [sp, #64]
	stp	x27, x28, [sp, #80]
	mov	x19, x0
	ldp	w20, w21, [x19, #0]
	ldp	w22, w23, [x19, #8]
	ldp	w24, w25, [x19, #16]
	ldp	w26, w27, [x19, #24]
	ldr	w28, [x19, #32]
	bl	.Lreload
	ldr	w0, [x19, #36]
	cbz	w0, .Ld084_body
	cmp	w0, #1
	b.eq	.Ld08c_body
	cmp	w0, #2
	b.eq	.Ld0ac_body
	b	.Ld0b4_body

	// d084: the strip's vertex count
.Ld084:
	tbnz	w28, #31, .Lstop_d084
	ldr	w0, [x19, #40]
	sub	w28, w28, w0
.Ld084_body:
	lsr	w0, w23, #29
	cmp	w0, #7
	b.eq	.Lbail_d084
	ubfx	w0, w23, #26, #3
	cmp	w0, #3
	b.ne	.Lbail_d084
	and	w0, w23, w9
	ldr	w21, [x8, w0, uxtw]
	add	w23, w23, #4
	cbnz	w21, .Ld08c
	mov	w0, #4				// d0c0, T = 1
	b	.Lout

	// d08c: a vertex to the store queue
.Ld08c:
	tbnz	w28, #31, .Lstop_d08c
	ldr	w0, [x19, #44]
	sub	w28, w28, w0
.Ld08c_body:
	// R8 in main RAM, R0 a store queue's 32 bytes, the vertex's 36 bytes in one 4 KB of the on-chip RAM
	and	w0, w23, w9
	lsr	w2, w23, #29
	ldr	w1, [x8, w0, uxtw]		// R10, the vertex (read early; used once the checks pass)
	cmp	w2, #7
	b.eq	.Lbail_d08c
	ubfx	w2, w23, #26, #3
	cmp	w2, #3
	b.ne	.Lbail_d08c
	lsr	w2, w20, #26
	cmp	w2, #0x38
	b.ne	.Lbail_d08c
	tst	w20, #0x1f
	b.ne	.Lbail_d08c
	lsr	w2, w1, #26
	cmp	w2, #0x1f
	b.ne	.Lbail_d08c
	cbz	x10, .Lbail_d08c
	and	w2, w1, #0xfff
	tst	w1, #3
	b.ne	.Lbail_d08c
	cmp	w2, #0xfdc
	b.hi	.Lbail_d08c
	lsr	w3, w1, w11
	and	w3, w3, #0x1000
	orr	w3, w3, w2
	add	x3, x10, x3
	ldp	w4, w5, [x3, #12]
	ldp	w6, w25, [x3, #20]		// R11 <- @(24,R10)
	ldp	w26, w27, [x3, #28]		// R12 <- @(28,R10), R13 <- @(32,R10)
	and	x0, x20, #0x20
	add	x0, x12, x0
	mov	w24, w1
	str	w22, [x0]
	stp	w4, w5, [x0, #4]
	str	w6, [x0, #12]
	str	w27, [x0, #16]
	stp	w25, w26, [x0, #24]
	add	w23, w23, #4
	subs	w21, w21, #1
	b.eq	.Ld0b4

	// d0ac: the store queue to the TA
.Ld0ac:
	tbnz	w28, #31, .Lstop_d0ac
	ldr	w0, [x19, #48]
	sub	w28, w28, w0
.Ld0ac_body:
	lsr	w0, w20, #26
	cmp	w0, #0x38
	b.ne	.Lbail_d0ac
	mov	w0, w20
	bl	.Lflush
	add	w20, w20, #32
	b	.Ld08c

	// d0b4: end of strip
.Ld0b4:
	tbnz	w28, #31, .Lstop_d0b4
	ldr	w0, [x19, #52]
	sub	w28, w28, w0
.Ld0b4_body:
	lsr	w0, w20, #26
	cmp	w0, #0x38
	b.ne	.Lbail_d0b4
	ldr	w27, [x19, #56]
	and	x0, x20, #0x3c
	str	w27, [x12, x0]
	mov	w0, w20
	bl	.Lflush
	add	w20, w20, #32
	b	.Ld084

	// a block's start, its cycles run out: the next timeslice if the scheduler has nothing to do, else stopped there
.Lstop_d084:
	bl	.Ltimeslice
	cbnz	w0, .Ld084
	mov	w0, #0
	b	.Lout
.Lstop_d08c:
	bl	.Ltimeslice
	cbnz	w0, .Ld08c
	mov	w0, #1
	b	.Lout
.Lstop_d0ac:
	bl	.Ltimeslice
	cbnz	w0, .Ld0ac
	mov	w0, #2
	b	.Lout
.Lstop_d0b4:
	bl	.Ltimeslice
	cbnz	w0, .Ld0b4
	mov	w0, #3
	b	.Lout
	// a block's fast path not taken: its cycles back, stopped at its start untouched (0x100: bailed)
.Lbail_d084:
	ldr	w1, [x19, #40]
	add	w28, w28, w1
	mov	w0, #0x100
	b	.Lout
.Lbail_d08c:
	ldr	w1, [x19, #44]
	add	w28, w28, w1
	mov	w0, #0x101
	b	.Lout
.Lbail_d0ac:
	ldr	w1, [x19, #48]
	add	w28, w28, w1
	mov	w0, #0x102
	b	.Lout
.Lbail_d0b4:
	ldr	w1, [x19, #52]
	add	w28, w28, w1
	mov	w0, #0x103

.Lout:
	stp	w20, w21, [x19, #0]
	stp	w22, w23, [x19, #8]
	stp	w24, w25, [x19, #16]
	stp	w26, w27, [x19, #24]
	str	w28, [x19, #32]
	ldp	x19, x20, [sp, #16]
	ldp	x21, x22, [sp, #32]
	ldp	x23, x24, [sp, #48]
	ldp	x25, x26, [sp, #64]
	ldp	x27, x28, [sp, #80]
	ldp	x29, x30, [sp], #96
	ret

	// x8-x17 from the SubmitIo and what it points to
.Lreload:
	ldr	x8, [x19, #64]
	ldr	w9, [x19, #60]
	ldr	x10, [x19, #72]
	ldr	w0, [x19, #84]
	cmp	w0, #0
	csel	x10, x10, xzr, ne
	ldr	w11, [x19, #80]
	ldr	x12, [x19, #88]
	ldr	x13, [x19, #96]
	ldr	x14, [x19, #112]
	ldr	x15, [x19, #136]
	ldr	x0, [x19, #120]
	ldr	x16, [x14, x0]
	ldr	x0, [x19, #128]
	add	x17, x16, x0
	sub	x17, x17, #64
	ldr	x0, [x19, #104]
	ldr	x0, [x0]
	cmp	x0, #0
	csel	x17, x17, xzr, ne
	ret

	// arm64_intc_sched's common case: the CPU running, no interrupt pending, no event due in the next timeslice
.Ltimeslice:
	ldr	x1, [x19, #184]
	ldr	w2, [x1]
	cbz	w2, 1f
	ldr	x1, [x19, #176]
	ldr	w2, [x1]
	cbnz	w2, 1f
	ldr	x1, [x19, #168]
	ldr	w2, [x1]
	subs	w2, w2, #448
	b.mi	1f
	str	w2, [x1]
	add	w28, w28, #448
	mov	w0, #1
	ret
1:	mov	w0, #0
	ret

	// the dynarec's sqFlush: w0 the store queue address. To the TA if its area is the TA's and the TA's data neither at
	// its root nor full (nor without a context: x17 0); else doSqWrite
.Lflush:
	ubfx	w1, w0, #20, #6
	ldrb	w1, [x13, x1]
	ldr	x2, [x14]
	cbz	w1, .Lflush_slow
	cmp	x2, x16
	b.eq	.Lflush_slow
	cmp	x2, x17
	b.hs	.Lflush_slow
	ubfx	w1, w0, #5, #1
	add	x1, x12, x1, lsl #5
	ldp	q0, q1, [x1]
	ldr	w3, [x1]
	ldrb	w4, [x15, #2048]
	stp	q0, q1, [x2], #32
	str	x2, [x14]
	lsr	w5, w3, #29
	ubfx	w3, w3, #2, #5
	orr	w3, w3, w5, lsl #5
	orr	w3, w3, w4, lsl #8
	ldrb	w4, [x15, x3]
	strb	w4, [x15, #2048]
	tst	w4, #0xf0
	b.ne	.Lflush_handle
	ret
.Lflush_handle:
	stp	x30, xzr, [sp, #-16]!
	mov	w0, w4
	ldr	x5, [x19, #144]
	blr	x5
	bl	.Lreload
	ldp	x30, xzr, [sp], #16
	ret
.Lflush_slow:
	stp	x30, xzr, [sp, #-16]!
	ldr	x1, [x19, #160]
	ldr	x5, [x19, #152]
	blr	x5
	bl	.Lreload
	ldp	x30, xzr, [sp], #16
	ret
	.size deadcast_submit, .-deadcast_submit
)");
