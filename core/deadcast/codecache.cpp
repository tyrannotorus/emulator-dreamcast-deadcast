#include "codecache.h"
#include "types.h"
#include "cfg/option.h"
#include "hw/sh4/dyna/blockmanager.h"
#include "hw/sh4/dyna/ngen.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/sh4_if.h"
#include "hw/sh4/modules/mmu.h"
#include "hw/sh4/modules/wince.h"
#include "log/Log.h"
#include "oslib/oslib.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

namespace deadcast::codecache
{
namespace
{

// a list's first line; one without it, from an older version, can list blocks whose code changed and is not used.
// 3: the block's physical address too, for the MMU's games
constexpr char Header[] = "deadcast blocks 3\n";

struct Entry
{
	u32 vaddr;
	u32 paddr;		// where its code is (vaddr without the MMU)
	u32 fpscr;
	u32 size;		// its SH4 code's bytes
	u64 hash;		// of them
};

// translated code: what the list can be of (Deadcast: under the MMU too, Windows CE's games)
bool usable()
{
	return config::DynarecEnabled;
}

// Deadcast: under the MMU, whether the code from vaddr on (size bytes, and some past its end, which decoding may read)
// is at paddr on as the MMU stands, in RAM; its translated pages then pinned (mmuPinnedPages) for compiling it, which
// raises no exception and changes nothing. A page is translated as the MMU's lookup would, side effects aside: from the
// TLB (mmu_peek), else Windows CE's page tables (wince_resolve_address, which the lookup falls back on).
bool mappedAsBefore(u32 vaddr, u32 paddr, u32 size)
{
	mmuPinnedPages.count = 0;
	if (!mmu_enabled())
		return vaddr == paddr;
	const u32 first = vaddr & ~0xfffu;
	const u32 end = vaddr + size + 64;
	for (u32 page = first; page < end && page >= first; page += 0x1000)
	{
		u32 pa;
		bool pin = false;
		if (fast_reg_lut[page >> 29] != 0)
			pa = page;
		else if (mmu_peek(page, pa))
			pin = true;
		else
		{
#if defined(FAST_MMU) && defined(USE_WINCE_HACK)
			TLB_Entry entry;
			if (!wince_resolve_address(page, entry))
				return false;
			const u32 mask = mmu_mask[entry.Data.SZ1 * 2 + entry.Data.SZ0];
			pa = (entry.Data.PPN << 10) | (page & ~mask);
			pin = true;
#else
			return false;
#endif
		}
		const u32 want = (paddr & ~0xfffu) + (page - first);
		if ((pa & ~0xfffu) != want || (pa & 0x1C000000) != 0x0C000000)
			return false;
		if (pin)
		{
			if (mmuPinnedPages.count == std::size(mmuPinnedPages.vpage))
				return false;
			mmuPinnedPages.vpage[mmuPinnedPages.count] = page;
			mmuPinnedPages.ppage[mmuPinnedPages.count] = pa & ~0xfffu;
			mmuPinnedPages.count++;
		}
	}
	return true;
}

std::string file()
{
	std::string path = hostfs::getSavestatePath(0, true);
	const std::string ext = ".state";
	if (path.size() > ext.size() && path.compare(path.size() - ext.size(), ext.size(), ext) == 0)
		path.resize(path.size() - ext.size());
	return path + ".blocks";
}

bool inMainRam(u32 addr)
{
	return (addr & 0x1C000000) == 0x0C000000;
}

// FNV-1a over the block's SH4 code in main RAM; 0 if it is not all there
u64 hashOf(u32 addr, u32 size)
{
	const u32 offset = addr & RAM_MASK;
	if (!inMainRam(addr) || size == 0 || (u64)offset + size > (u64)RAM_MASK + 1)
		return 0;
	u64 h = 0xcbf29ce484222325ull;
	for (u32 i = 0; i < size; i++)
	{
		h ^= mem_b[offset + i];
		h *= 0x100000001b3ull;
	}
	return h;
}

}	// namespace

void save()
{
	if (!usable())
		return;
	std::vector<Entry> list;
	bm_ForEachBlock([&list](const RuntimeBlockInfo& block) {
		// protected: its code is still what it was compiled from
		if (!block.read_only || block.temp_block || !inMainRam(block.addr) || block.sh4_code_size == 0)
			return;
		const u64 hash = hashOf(block.addr, block.sh4_code_size);
		if (hash != 0)
			list.push_back({ block.vaddr, block.addr, block.fpu_cfg.full, block.sh4_code_size, hash });
	});
	if (list.empty())
		return;
	const std::string path = file();
	const std::string temp = path + ".new";
	FILE *f = std::fopen(temp.c_str(), "w");
	if (f == nullptr)
		return;
	std::fputs(Header, f);
	for (const Entry& e : list)
		std::fprintf(f, "%08x %08x %08x %x %016llx\n", e.vaddr, e.paddr, e.fpscr, e.size, (unsigned long long)e.hash);
	const bool written = std::fflush(f) == 0;
	std::fclose(f);
	if (written && std::rename(temp.c_str(), path.c_str()) == 0)
		NOTICE_LOG(DYNAREC, "Deadcast: %zu blocks kept for the next state load (%s)", list.size(), path.c_str());
	else
		std::remove(temp.c_str());
}

void warm()
{
	if (!usable())
		return;
	const std::string path = file();
	FILE *f = std::fopen(path.c_str(), "r");
	if (f == nullptr)
		return;
	char header[sizeof(Header)] = "";
	if (std::fgets(header, sizeof(header), f) == nullptr || std::string(header) != Header)
	{
		std::fclose(f);
		NOTICE_LOG(DYNAREC, "Deadcast: %s is from an older version, not used", path.c_str());
		return;
	}
	const auto start = std::chrono::steady_clock::now();
	// compiled as while the SH4 runs: with the CPU stopped (as at a state load) a block's exits go back to
	// the main loop for good instead of to the stubs that link them on first use (RelinkBlock)
	const u32 running = Sh4cntx.CpuRunning;
	Sh4cntx.CpuRunning = 1;
	// Deadcast: under the MMU, the CPU as loaded is left as it was: the cycles the MMU's lookups charge given back,
	// and SR.FD clear while decoding (the decoder raises the FPU's exception for it; the blocks check it as they run)
	const s32 cycles = Sh4cntx.cycle_counter;
	const u32 fd = Sh4cntx.sr.FD;
	if (mmu_enabled())
		Sh4cntx.sr.FD = 0;
	u32 compiled = 0, skipped = 0;
	unsigned vaddr, paddr, fpscr, size;
	unsigned long long hash;
	while (std::fscanf(f, "%x %x %x %x %llx", &vaddr, &paddr, &fpscr, &size, &hash) == 5)
	{
		// the code it was compiled from, where it was, else not decoded (a state from elsewhere in the game holds
		// other code or data, or the MMU maps it elsewhere)
		const bool done = mappedAsBefore(vaddr, paddr, size) && hashOf(paddr, size) == hash
				&& rdv_Precompile(vaddr, paddr, fpscr);
		mmuPinnedPages.count = 0;
		(done ? compiled : skipped)++;
	}
	std::fclose(f);
	Sh4cntx.sr.FD = fd;
	Sh4cntx.cycle_counter = cycles;
	Sh4cntx.CpuRunning = running;
	const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
	NOTICE_LOG(DYNAREC, "Deadcast: %u blocks compiled ahead in %lld ms (%u not: changed, compiled already or no room)",
			compiled, (long long)ms, skipped);
}

}
