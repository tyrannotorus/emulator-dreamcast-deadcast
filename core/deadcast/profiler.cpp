#include "profiler.h"
#include <array>
#include "types.h"
#include "hw/sh4/dyna/blockmanager.h"
#include "hw/sh4/dyna/ngen.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/sh4_opcode_list.h"

#include <map>
#include <vector>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <pthread.h>
#include <signal.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

namespace
{

constexpr int MaxThreads = 4;
constexpr size_t MaxSamples = 1 << 18;	// a thread's first four minutes or so

constexpr int StackDepth = 8;

struct Sample
{
	uintptr_t pc;
	uintptr_t lr;
	u32 ms;		// since the profiler started
	uintptr_t stack[StackDepth];	// return addresses up the frame records, 0 past the last
};

struct ThreadSamples
{
	char name[16];
	timer_t timer;
	uintptr_t stackLo, stackHi;	// the thread's stack, for walking its frame records safely
	std::atomic<size_t> count;
	Sample samples[MaxSamples];
};

const char *const outPath = std::getenv("DEADCAST_PROF");
u64 startNs;

u32 sinceStartMs()
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);	// async-signal-safe
	return (u32)(((u64)ts.tv_sec * 1000000000 + ts.tv_nsec - startNs) / 1000000);
}
ThreadSamples *threads[MaxThreads];
std::atomic<int> threadCount;
std::atomic<bool> stopped;
thread_local ThreadSamples *current;

void onSample(int, siginfo_t *, void *context)
{
	ThreadSamples *t = current;
	if (t == nullptr || stopped.load(std::memory_order_relaxed))
		return;
	size_t i = t->count.load(std::memory_order_relaxed);
	if (i >= MaxSamples)
		return;
#if defined(__aarch64__)
	const mcontext_t& mc = static_cast<ucontext_t *>(context)->uc_mcontext;
	Sample& s = t->samples[i];
	s = { (uintptr_t)mc.pc, (uintptr_t)mc.regs[30], sinceStartMs(), {} };
	// the frame records: x29 points at {previous x29, return address}
	uintptr_t fp = mc.regs[29];
	for (int d = 0; d < StackDepth; d++)
	{
		if (fp < t->stackLo || fp + 16 > t->stackHi || (fp & 7) != 0)
			break;
		const uintptr_t *frame = (const uintptr_t *)fp;
		s.stack[d] = frame[1];
		if (frame[0] <= fp)
			break;
		fp = frame[0];
	}
#elif defined(__x86_64__)
	const mcontext_t& mc = static_cast<ucontext_t *>(context)->uc_mcontext;
	t->samples[i] = { (uintptr_t)mc.gregs[REG_RIP], 0, sinceStartMs() };
#else
	t->samples[i] = { 0, 0, 0 };
#endif
	t->count.store(i + 1, std::memory_order_relaxed);
}

}

// how many times the blocks compiled ran: 1, 2, 3-4, 5-8, ... (bucket = bit length)
static std::array<std::atomic<u64>, 24> runsHistogram;

void profilerBlockGone(u64 runs)
{
	if (!profilerCountsRuns())
		return;
	int bucket = runs == 0 ? 0 : std::min(23, 64 - __builtin_clzll(runs));
	runsHistogram[bucket]++;
}

// DEADCAST_PROF_NORUNS: not counted, the blocks' code as it runs unprofiled (counting costs small blocks up to 40%)
bool profilerCountsRuns()
{
	static const bool noRuns = std::getenv("DEADCAST_PROF_NORUNS") != nullptr;
	return outPath != nullptr && !noRuns;
}

void profilerStartThread(const char *name)
{
	if (outPath == nullptr)
		return;
	static std::once_flag installed;
	std::call_once(installed, [] {
		timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		startNs = (u64)ts.tv_sec * 1000000000 + ts.tv_nsec;
		struct sigaction sa {};
		sa.sa_sigaction = onSample;
		sa.sa_flags = SA_SIGINFO | SA_RESTART;
		sigemptyset(&sa.sa_mask);
		sigaction(SIGPROF, &sa, nullptr);
	});
	int index = threadCount.fetch_add(1);
	if (index >= MaxThreads)
		return;
	ThreadSamples *t = new ThreadSamples();
	strncpy(t->name, name, sizeof(t->name) - 1);
	pthread_attr_t attr;
	if (pthread_getattr_np(pthread_self(), &attr) == 0)
	{
		void *addr;
		size_t size;
		if (pthread_attr_getstack(&attr, &addr, &size) == 0)
		{
			t->stackLo = (uintptr_t)addr;
			t->stackHi = (uintptr_t)addr + size;
		}
		pthread_attr_destroy(&attr);
	}
	current = t;
	threads[index] = t;

	sigevent sev {};
	sev.sigev_notify = SIGEV_THREAD_ID;
	sev.sigev_signo = SIGPROF;
	sev._sigev_un._tid = (pid_t)syscall(SYS_gettid);
	// DEADCAST_PROF_WALL: a sample every millisecond of wall time instead, where the thread waits too
	const clockid_t clock = std::getenv("DEADCAST_PROF_WALL") != nullptr ? CLOCK_MONOTONIC : CLOCK_THREAD_CPUTIME_ID;
	if (timer_create(clock, &sev, &t->timer) != 0)
	{
		perror("Deadcast profiler: timer_create");
		return;
	}
	itimerspec every {};
	every.it_interval.tv_nsec = 1000000;
	every.it_value.tv_nsec = 1000000;
	timer_settime(t->timer, 0, &every, nullptr);
}

void profilerDump()
{
	if (outPath == nullptr || stopped.exchange(true))
		return;
	int count = std::min(threadCount.load(), MaxThreads);
	for (int i = 0; i < count; i++)
		if (threads[i] != nullptr)
			timer_delete(threads[i]->timer);

	FILE *f = fopen(outPath, "w");
	if (f == nullptr)
	{
		perror(outPath);
		return;
	}
	// "map <line of /proc/self/maps>": where the executable is loaded
	if (FILE *maps = fopen("/proc/self/maps", "r"))
	{
		char line[512];
		while (fgets(line, sizeof(line), maps))
			fprintf(f, "map %s", line);
		fclose(maps);
	}
	uintptr_t codeStart, codeEnd;
	rdv_codeRange(codeStart, codeEnd);
	fprintf(f, "code %zx %zx\n", (size_t)codeStart, (size_t)codeEnd);
	// "bytes <hex>": the start of the translated code, the main loop and stubs, to disassemble
	fprintf(f, "bytes ");
	for (size_t i = 0; i < 4096; i++)
		fprintf(f, "%02x", ((const u8 *)codeStart)[i]);
	fprintf(f, "\n");
	// "s <thread> <pc> <lr> <block vaddr> <block code> <ms> <return addresses...>": in translated
	// code, the SH4 block (vaddr 0 and code 0 for the code that is not a block's, like the main
	// loop); when, in milliseconds since the profiler started; the return addresses up the frame
	// records
	for (int i = 0; i < count; i++)
	{
		ThreadSamples *t = threads[i];
		if (t == nullptr)
			continue;
		size_t n = t->count.load();
		for (size_t j = 0; j < n; j++)
		{
			const Sample& s = t->samples[j];
			u32 vaddr = 0;
			uintptr_t code = 0;
			if (s.pc >= codeStart && s.pc < codeEnd)
				if (RuntimeBlockInfoPtr block = bm_GetBlock((void *)s.pc))
				{
					vaddr = block->vaddr;
					code = (uintptr_t)block->code;
				}
			fprintf(f, "s %s %zx %zx %x %zx %u", t->name, (size_t)s.pc, (size_t)s.lr, vaddr, (size_t)code, s.ms);
			for (int d = 0; d < StackDepth && s.stack[d] != 0; d++)
				fprintf(f, " %zx", (size_t)s.stack[d]);
			fprintf(f, "\n");
		}
	}
	// "block <vaddr> <addr> <code> <host bytes hex>", then "sh4 <vaddr> <pc> <disassembly>": the
	// hottest blocks, to see what in them takes the time
	std::map<RuntimeBlockInfo *, size_t> hits;
	for (int i = 0; i < count; i++)
	{
		ThreadSamples *t = threads[i];
		if (t == nullptr)
			continue;
		size_t n = t->count.load();
		for (size_t j = 0; j < n; j++)
			if (t->samples[j].pc >= codeStart && t->samples[j].pc < codeEnd)
				if (RuntimeBlockInfoPtr block = bm_GetBlock((void *)t->samples[j].pc))
					hits[block.get()]++;
	}
	std::vector<std::pair<size_t, RuntimeBlockInfo *>> hottest;
	for (const auto& [block, n] : hits)
		hottest.emplace_back(n, block);
	std::sort(hottest.rbegin(), hottest.rend());
	if (hottest.size() > 150)
		hottest.resize(150);
	// "run <vaddr> <addr> <runs> <guest cycles> <sh4 instructions>": where the SH4's time went, the
	// blocks that took the most guest cycles, with their SH4 code ("sh4" lines, no host code)
	std::vector<RuntimeBlockInfoPtr> all = bm_AllBlocks();
	// "runs <bucket> <blocks>": how many times the blocks compiled ran (gone ones and those still
	// there): bucket b for 2^(b-1) <= runs < 2^b, 0 for never
	{
		std::array<u64, 24> histogram;
		for (size_t b = 0; b < histogram.size(); b++)
			histogram[b] = runsHistogram[b].load();
		for (const RuntimeBlockInfoPtr& block : all)
			histogram[block->runs == 0 ? 0 : std::min(23, 64 - __builtin_clzll(block->runs))]++;
		for (size_t b = 0; b < histogram.size(); b++)
			fprintf(f, "runs %zu %llu\n", b, (unsigned long long)histogram[b]);
	}
	std::sort(all.begin(), all.end(), [](const RuntimeBlockInfoPtr& a, const RuntimeBlockInfoPtr& b) {
		return a->runs * a->guest_cycles > b->runs * b->guest_cycles;
	});
	for (size_t i = 0; i < all.size() && i < 400 && all[i]->runs != 0; i++)
	{
		const RuntimeBlockInfoPtr& block = all[i];
		fprintf(f, "run %x %x %llu %u %u\n", block->vaddr, block->addr, (unsigned long long)block->runs,
				block->guest_cycles, block->guest_opcodes);
		if (std::any_of(hottest.begin(), hottest.end(), [&](const auto& h) { return h.second == block.get(); }))
			continue;
		for (u32 offset = 0; offset < block->sh4_code_size; offset += 2)
		{
			u16 op = ReadMem16_nommu(block->addr + offset);
			char text[128];
			OpDesc[op]->Disassemble(text, block->vaddr + offset, op);
			fprintf(f, "sh4 %x %x %s\n", block->vaddr, block->vaddr + offset, text);
		}
	}
	for (const auto& [n, block] : hottest)
	{
		fprintf(f, "block %x %x %zx ", block->vaddr, block->addr, (size_t)CC_RW2RX(block->code));
		for (u32 i = 0; i < block->host_code_size; i++)
			fprintf(f, "%02x", ((const u8 *)block->code)[i]);
		fprintf(f, "\n");
		for (u32 offset = 0; offset < block->sh4_code_size; offset += 2)
		{
			u16 op = ReadMem16_nommu(block->addr + offset);
			char text[128];
			OpDesc[op]->Disassemble(text, block->vaddr + offset, op);
			fprintf(f, "sh4 %x %x %s\n", block->vaddr, block->vaddr + offset, text);
		}
	}
	fclose(f);
	NOTICE_LOG(COMMON, "Deadcast profiler: samples written to %s", outPath);
}
