// Deadcast: the SH4's MMU address space mapped into the host's. Each ASID gets a 4 GB window of host
// address space (16 windows, the least recently used one reassigned) where RAM pages are mapped at
// their virtual address on first access, so translated code reaches them with one load or store at
// [window + vaddr] (x27 in the ARM64 dynarec). Other accesses fault, and the fault handler maps the
// page or sends the access through a stub (rec_arm64.cpp). Mappings last until the MMU flushes;
// code write protection (bm_LockPage) covers them too. Below each window are its dispatch cache, the
// entries filled since its last flush, and its slot 0 code page table, all reached from x27.
#pragma once
#include "types.h"

namespace mmuwin
{
constexpr u32 DispatchCacheOffset = 0x10000;	// the cache at current - this
constexpr u32 DispatchFilledOffset = 0x12000;	// its MmuFilled at current - this
constexpr u32 CodeLutOffset = 0x22000;			// the code pages of slot 0 at current - this

// the current ASID's window (the dynarec's x27), null without one
extern u8 *current;

bool init();
void term();
// the ASID changed: its window becomes current
void switchAsid(u32 asid);
// the MMU's translations changed: no window keeps any page
void flushAll();
// whether host is in the current window: vaddr then the virtual address it stands for
bool inWindow(const void *host, u32& vaddr);
// maps the RAM page of paddr at vaddr in the current window (read only while its code is protected)
void mapPage(u32 vaddr, u32 paddr);
// whether the current window has the page of vaddr mapped
bool isMapped(u32 vaddr);
// the code write protection of the RAM page at ramOffset: its mappings follow it
void protectRamPage(u32 ramOffset, bool locked);
void unprotectAll();
// the blocks changed: every window's dispatch cache is emptied
void flushDispatch();
// the current window's code page table for slot 0 (mmuAddressLUT's format), null without one
uintptr_t *codeLut();
}
