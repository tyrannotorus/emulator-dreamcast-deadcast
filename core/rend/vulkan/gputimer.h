// Deadcast: the GPU's time over a submission's work, from two bottom-of-pipe timestamps around it, read
// back once its fence has signalled; a pair of queries per frame in flight. Only with DEADCAST_PERF.
#pragma once
#include "vulkan.h"
#include "hw/pvr/spg.h"
#include "log/Log.h"

#include <vector>

class GpuTimer
{
public:
	// kind: what perfGpu is told it measured (spg.h)
	void init(vk::PhysicalDevice physicalDevice, vk::Device device, u32 queueFamily, u32 slots, int kind)
	{
		pool.reset();
		if (!perfOn())
			return;
		const std::vector<vk::QueueFamilyProperties> families = physicalDevice.getQueueFamilyProperties();
		const u32 bits = queueFamily < families.size() ? families[queueFamily].timestampValidBits : 0;
		const float period = physicalDevice.getProperties().limits.timestampPeriod;
		if (bits == 0 || period <= 0.f)
		{
			NOTICE_LOG(RENDERER, "GpuTimer: no timestamps on this queue (%u bits, %.1f ns)", bits, period);
			return;
		}
		this->device = device;
		this->kind = kind;
		nsPerTick = period;
		mask = bits >= 64 ? ~0ull : (1ull << bits) - 1;
		pool = device.createQueryPoolUnique(vk::QueryPoolCreateInfo(vk::QueryPoolCreateFlags(), vk::QueryType::eTimestamp, slots * 2));
		pending.assign(slots, false);
		NOTICE_LOG(RENDERER, "GpuTimer %d: %u slots, timestamps %u bits, %.2f ns a tick", kind, slots, bits, period);
	}

	void term() {
		pool.reset();
	}

	bool active() const {
		return (bool)pool;
	}

	// first in the slot's submission
	void start(vk::CommandBuffer commandBuffer, u32 slot)
	{
		commandBuffer.resetQueryPool(*pool, slot * 2, 2);
		commandBuffer.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, *pool, slot * 2);
		pending[slot] = true;
	}

	// last in it
	void stop(vk::CommandBuffer commandBuffer, u32 slot) {
		commandBuffer.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, *pool, slot * 2 + 1);
	}

	// the slot's last measure, its fence waited for
	void collect(u32 slot)
	{
		if (!pool || slot >= pending.size() || !pending[slot])
			return;
		pending[slot] = false;
		u64 ticks[2];
		if (device.getQueryPoolResults(*pool, slot * 2, 2, sizeof(ticks), ticks, sizeof(u64), vk::QueryResultFlagBits::e64)
				!= vk::Result::eSuccess)
			return;
		perfGpu(kind, (unsigned long long)((double)((ticks[1] - ticks[0]) & mask) * nsPerTick / 1000.0));
	}

private:
	vk::Device device;
	vk::UniqueQueryPool pool;
	std::vector<bool> pending;
	double nsPerTick = 0;
	u64 mask = 0;
	int kind = 0;
};
