#pragma once
#include "types.h"

namespace addrspace
{

//Typedef's
//ReadMem
typedef u8 DYNACALL ReadMem8FP(u32 Address);
typedef u16 DYNACALL ReadMem16FP(u32 Address);
typedef u32 DYNACALL ReadMem32FP(u32 Address);
//WriteMem
typedef void DYNACALL WriteMem8FP(u32 Address,u8 data);
typedef void DYNACALL WriteMem16FP(u32 Address,u16 data);
typedef void DYNACALL WriteMem32FP(u32 Address,u32 data);

//our own handle type
typedef u32 handler;

//Functions

//init/reset/term
void init();
void term();
void initMappings();

//functions to register and map handlers/memory
handler registerHandler(ReadMem8FP *read8, ReadMem16FP *read16, ReadMem32FP *read32, WriteMem8FP *write8, WriteMem16FP *write16, WriteMem32FP *write32);

#define addrspaceRegisterHandlerTemplate(read, write) addrspace::registerHandler \
									(read<u8>, read<u16>, read<u32>,	\
									write<u8>, write<u16>, write<u32>)

void mapHandler(handler Handler, u32 start, u32 end);
void mapBlock(void* base, u32 start, u32 end, u32 mask);
void mirrorMapping(u32 new_region, u32 start, u32 size);

static inline void mapBlockMirror(void *base, u32 start, u32 end, u32 blck_size)
{
	u32 block_size = blck_size >> 24;
	for (u32 _maip = start; _maip <= end; _maip += block_size)
		mapBlock(base, _maip, _maip + block_size - 1, blck_size - 1);
}

u8 DYNACALL read8(u32 address);
u16 DYNACALL read16(u32 address);
u32 DYNACALL read32(u32 address);
u64 DYNACALL read64(u32 address);
template<typename T> T DYNACALL readt(u32 addr);

static inline int32_t DYNACALL read8SX32(u32 address) {
	return (int32_t)(int8_t)readt<u8>(address);
}
static inline int32_t DYNACALL read16SX32(u32 address) {
	return (int32_t)(int16_t)readt<u16>(address);
}

void DYNACALL write8(u32 address, u8 data);
void DYNACALL write16(u32 address, u16 data);
void DYNACALL write32(u32 address, u32 data);
void DYNACALL write64(u32 address, u64 data);
template<typename T> void DYNACALL writet(u32 addr, T data);

//should be called at start up to ensure it will succeed
bool reserve();
void release();

//dynarec helpers
void *readConst(u32 addr, bool& ismem, u32 sz);
void *writeConst(u32 addr, bool& ismem, u32 sz);

extern u8* ram_base;

static inline bool virtmemEnabled() {
	return ram_base != nullptr;
}
void bm_reset(); // FIXME rename? move?
bool bm_lockedWrite(u8* address); // FIXME rename?

// Deadcast: the SH4's on-chip RAM in the dynarec's memory map, as CCR has it (addrspace.cpp)
void mapOnChipRam(bool indexBit25);

// Deadcast: the dynarec's window on the SH4's address space without the MMU: 4 GB, where RAM, AICA RAM
// and VRAM (its 64-bit path) are mapped at their addresses in P0, P1 and P2, and the on-chip RAM at
// 7C000000, an access a single load or store at window + address; everything else faults (the
// dynarec's access stubs). Null without virtual memory.
extern u8 *window;
// where in the 512 MB map (ram_base) the window address p is, else null
u8 *windowToMap(void *p);
// whether the window address p is in an alias the window maps (P0, P1, P2): only there is a fault
// a protection one (RAM's code, VRAM's textures), elsewhere (U0's mirrors, P3) nothing is mapped
bool windowAliasMapped(void *p);
// main RAM's code protection (bm_LockPage), in the window's aliases of it
void protectRamInWindow(u32 offset, u32 size, bool locked);
// Deadcast: the SH4's views of the AICA's RAM (area 0, and the window's aliases of it) gone (on), so that
// every SH4 access to it faults and the dynarec turns it into a call of its handler (sb_mem.cpp), or back
// read only (initMappings maps them so as well): a netplay session's AICA thread in step (aica.cpp)
void aramThroughHandlers(bool on);

void protectVram(u32 addr, u32 size);
void unprotectVram(u32 addr, u32 size);
u32 getVramOffset(void *addr);
void getAddress(void** out_ram_base, void** out_ram, void** out_vram, void** out_aica);

} // namespace addrspace
