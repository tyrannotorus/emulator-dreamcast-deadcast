#include "codecache.h"
#include "types.h"
#include "cfg/option.h"
#include "hw/sh4/dyna/blockmanager.h"
#include "hw/sh4/dyna/ngen.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/sh4_if.h"
#include "hw/sh4/modules/mmu.h"
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

struct Entry
{
	u32 vaddr;
	u32 fpscr;
	u32 size;		// its SH4 code's bytes
	u64 hash;		// of them
};

// translated code without the MMU: what the list can be of
bool usable()
{
	return config::DynarecEnabled && !mmu_enabled();
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
		if (block.temp_block || !inMainRam(block.addr) || block.sh4_code_size == 0)
			return;
		const u64 hash = hashOf(block.addr, block.sh4_code_size);
		if (hash != 0)
			list.push_back({ block.vaddr, block.fpu_cfg.full, block.sh4_code_size, hash });
	});
	if (list.empty())
		return;
	const std::string path = file();
	const std::string temp = path + ".new";
	FILE *f = std::fopen(temp.c_str(), "w");
	if (f == nullptr)
		return;
	for (const Entry& e : list)
		std::fprintf(f, "%08x %08x %x %016llx\n", e.vaddr, e.fpscr, e.size, (unsigned long long)e.hash);
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
	FILE *f = std::fopen(file().c_str(), "r");
	if (f == nullptr)
		return;
	const auto start = std::chrono::steady_clock::now();
	// compiled as while the SH4 runs: with the CPU stopped (as at a state load) a block's exits go back to
	// the main loop for good instead of to the stubs that link them on first use (RelinkBlock)
	const u32 running = Sh4cntx.CpuRunning;
	Sh4cntx.CpuRunning = 1;
	u32 compiled = 0, skipped = 0;
	unsigned vaddr, fpscr, size;
	unsigned long long hash;
	while (std::fscanf(f, "%x %x %x %llx", &vaddr, &fpscr, &size, &hash) == 4)
	{
		const bool done = rdv_Precompile(vaddr, fpscr, [size, hash](u32 addr, u32 codeSize) {
			return codeSize == size && hashOf(addr, codeSize) == hash;
		});
		(done ? compiled : skipped)++;
	}
	std::fclose(f);
	Sh4cntx.CpuRunning = running;
	const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
	NOTICE_LOG(DYNAREC, "Deadcast: %u blocks compiled ahead in %lld ms (%u not: changed, compiled already or no room)",
			compiled, (long long)ms, skipped);
}

}
