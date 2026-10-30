#include "mmuwindows.h"
#include "hw/sh4/modules/mmu.h"
#include "oslib/virtmem.h"
#include "log/Log.h"

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

namespace mmuwin
{
namespace
{
constexpr int Windows = 16;
constexpr u64 WindowSize = 1ull << 32;
constexpr u64 Prefix = 0x30000;			// the dispatch cache and code page table below each window
constexpr u64 Stride = WindowSize + Prefix;
static_assert(sizeof(MmuDispatchEntry) * MmuDispatchEntries == DispatchCacheOffset);
static_assert(sizeof(MmuFilled) <= DispatchFilledOffset - DispatchCacheOffset);
static_assert(MmuSlot0Pages * sizeof(uintptr_t) == CodeLutOffset - DispatchFilledOffset && CodeLutOffset <= Prefix);
constexpr u32 PageBits = 12;
constexpr u32 Pages = 1u << (32 - PageBits);
constexpr u32 RamPages = (32u << 20) >> PageBits;	// up to 32 MB of RAM

struct Window
{
	int asid = -1;
	u64 lastUse = 0;
	std::vector<u8> mapped;							// a bit a page
	std::vector<std::pair<u32, u32>> pages;		// (virtual page, RAM page) mapped
};

u8 *space;
Window windows[Windows];
int asidWindow[256];
u64 uses;
int currentIndex = -1;
std::vector<u8 *> aliases[RamPages];	// where each RAM page is mapped in the windows
bool locked[RamPages];

u8 *windowBase(int i) {
	return space + (u64)i * Stride + Prefix;
}

MmuDispatchEntry *dispatchCache(int i) {
	return (MmuDispatchEntry *)(windowBase(i) - DispatchCacheOffset);
}

MmuFilled *dispatchFilled(int i) {
	return (MmuFilled *)(windowBase(i) - DispatchFilledOffset);
}

uintptr_t *codeLutOf(int i) {
	return (uintptr_t *)(windowBase(i) - CodeLutOffset);
}

// empties window i's dispatch cache: the entries filled since the last time, else all
void clearDispatch(int i, bool all)
{
	MmuDispatchEntry *cache = dispatchCache(i);
	MmuFilled *filled = dispatchFilled(i);
	if (!all && filled->count <= MmuFilled::Max)
		for (u32 j = 0; j < filled->count; j++)
			cache[filled->index[j]].vaddr = 1;
	else
		for (u32 j = 0; j < MmuDispatchEntries; j++)
			cache[j].vaddr = 1;
	filled->count = 0;
}

void clearWindow(int i)
{
	Window& w = windows[i];
	for (const auto& [vpn, ramPage] : w.pages)
	{
		std::vector<u8 *>& list = aliases[ramPage];
		u8 *host = windowBase(i) + ((u64)vpn << PageBits);
		list.erase(std::remove(list.begin(), list.end(), host), list.end());
		w.mapped[vpn >> 3] &= ~(1 << (vpn & 7));
	}
	if (!w.pages.empty())
		virtmem::unmap_to_reserved(windowBase(i), WindowSize);
	w.pages.clear();
	clearDispatch(i, false);
	memset(codeLutOf(i), 0, MmuSlot0Pages * sizeof(uintptr_t));
}
}

u8 *current;

bool init()
{
	if (space != nullptr)
		return true;
	space = (u8 *)virtmem::reserve_space(Windows * Stride);
	if (space == nullptr)
	{
		WARN_LOG(VMEM, "Deadcast: no room for the MMU windows");
		return false;
	}
	for (int i = 0; i < Windows; i++)
	{
		Window& w = windows[i];
		w.asid = -1;
		w.mapped.assign(Pages / 8, 0);
		w.pages.clear();
		virtmem::region_unlock(windowBase(i) - Prefix, Prefix);
		clearDispatch(i, true);
	}
	std::fill(std::begin(asidWindow), std::end(asidWindow), -1);
	currentIndex = -1;
	current = nullptr;
	NOTICE_LOG(VMEM, "Deadcast: MMU windows at %p", space);
	return true;
}

void term()
{
	if (space == nullptr)
		return;
	for (int i = 0; i < Windows; i++)
		clearWindow(i);
	virtmem::release_space(space, Windows * Stride);
	space = nullptr;
	current = nullptr;
	currentIndex = -1;
}

void switchAsid(u32 asid)
{
	if (space == nullptr)
		return;
	asid &= 0xff;
	int i = asidWindow[asid];
	if (i < 0)
	{
		// a free window, else the one used longest ago
		for (int j = 0; j < Windows && i < 0; j++)
			if (windows[j].asid < 0)
				i = j;
		if (i < 0)
		{
			i = 0;
			for (int j = 1; j < Windows; j++)
				if (windows[j].lastUse < windows[i].lastUse)
					i = j;
			asidWindow[windows[i].asid] = -1;
			clearWindow(i);
			clearDispatch(i, true);
		}
		windows[i].asid = asid;
		asidWindow[asid] = i;
	}
	windows[i].lastUse = ++uses;
	currentIndex = i;
	current = windowBase(i);
}

void flushAll()
{
	if (space == nullptr)
		return;
	for (int i = 0; i < Windows; i++)
		clearWindow(i);
}

bool inWindow(const void *host, u32& vaddr)
{
	const u8 *p = (const u8 *)host;
	if (current == nullptr || p < current || p >= current + WindowSize)
		return false;	// (not below it either: the dispatch cache is never protected)
	vaddr = (u32)(p - current);
	return true;
}

bool isMapped(u32 vaddr)
{
	if (currentIndex < 0)
		return false;
	const u32 vpn = vaddr >> PageBits;
	return (windows[currentIndex].mapped[vpn >> 3] & (1 << (vpn & 7))) != 0;
}

void mapPage(u32 vaddr, u32 paddr)
{
	if (currentIndex < 0 || isMapped(vaddr))
		return;
	Window& w = windows[currentIndex];
	const u32 vpn = vaddr >> PageBits;
	const u32 ramPage = (paddr & RAM_MASK) >> PageBits;
	u8 *host = current + ((u64)vpn << PageBits);
	if (!virtmem::map_ram_page(host, ramPage << PageBits, !locked[ramPage]))
	{
		WARN_LOG(VMEM, "Deadcast: cannot map RAM page %x at %08x", ramPage << PageBits, vaddr);
		return;
	}
	w.mapped[vpn >> 3] |= 1 << (vpn & 7);
	w.pages.emplace_back(vpn, ramPage);
	aliases[ramPage].push_back(host);
}

void protectRamPage(u32 ramOffset, bool lock)
{
	const u32 ramPage = (ramOffset & RAM_MASK) >> PageBits;
	if (locked[ramPage] == lock)
		return;
	locked[ramPage] = lock;
	for (u8 *host : aliases[ramPage])
		virtmem::protect_page(host, !lock);
}

uintptr_t *codeLut()
{
	return currentIndex < 0 ? nullptr : codeLutOf(currentIndex);
}

void flushDispatch()
{
	if (space == nullptr)
		return;
	for (int i = 0; i < Windows; i++)
		clearDispatch(i, false);
}

void unprotectAll()
{
	for (u32 page = 0; page < RamPages; page++)
		if (locked[page])
			protectRamPage(page << PageBits, false);
}

}
