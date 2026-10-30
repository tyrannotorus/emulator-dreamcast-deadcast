#include "allocator.h"
#include "log/Log.h"

#include <algorithm>

namespace
{
constexpr vk::DeviceSize BlockSize = 32 << 20;
constexpr vk::DeviceSize DedicatedSize = BlockSize / 4;

vk::DeviceSize alignUp(vk::DeviceSize v, vk::DeviceSize a) {
	return (v + a - 1) / a * a;
}
}

Allocation::~Allocation()
{
	if (allocator != nullptr)
		allocator->free(block, offset, size);
}

bool Allocation::IsHostVisible() const {
	return block->mapped != nullptr;
}

void *Allocation::MapMemory() const
{
	verify(block->mapped != nullptr);
	if (!block->coherent)
		allocator->device.invalidateMappedMemoryRanges(vk::MappedMemoryRange(block->memory, 0, VK_WHOLE_SIZE));
	return block->mapped + offset;
}

void Allocation::UnmapMemory() const
{
	if (!block->coherent)
		allocator->device.flushMappedMemoryRanges(vk::MappedMemoryRange(block->memory, 0, VK_WHOLE_SIZE));
}

void GpuAllocator::Init(vk::PhysicalDevice physicalDevice, vk::Device device)
{
	this->device = device;
	memoryProperties = physicalDevice.getMemoryProperties();
	granularity = std::max<vk::DeviceSize>(1, physicalDevice.getProperties().limits.bufferImageGranularity);
	for (u32 i = 0; i < memoryProperties.memoryTypeCount; i++)
		INFO_LOG(RENDERER, "Memory type %d: heap %d flags %s", i, memoryProperties.memoryTypes[i].heapIndex,
				vk::to_string(memoryProperties.memoryTypes[i].propertyFlags).c_str());
}

void GpuAllocator::Term()
{
	std::lock_guard<std::mutex> lock(mutex);
	for (auto& block : blocks)
	{
		if (block->used != 0)
			WARN_LOG(RENDERER, "GpuAllocator: %d allocations left in a block of type %d", block->used, block->type);
		if (block->mapped != nullptr)
			device.unmapMemory(block->memory);
		device.freeMemory(block->memory);
	}
	blocks.clear();
}

Allocation GpuAllocator::AllocateForImage(vk::Image image, vk::MemoryPropertyFlags required,
		vk::MemoryPropertyFlags preferred, vk::MemoryPropertyFlags avoided)
{
	Allocation a = allocate(device.getImageMemoryRequirements(image), required, preferred, avoided);
	device.bindImageMemory(image, a.block->memory, a.offset);
	return a;
}

Allocation GpuAllocator::AllocateForBuffer(vk::Buffer buffer, vk::MemoryPropertyFlags required,
		vk::MemoryPropertyFlags preferred, vk::MemoryPropertyFlags avoided)
{
	Allocation a = allocate(device.getBufferMemoryRequirements(buffer), required, preferred, avoided);
	device.bindBufferMemory(buffer, a.block->memory, a.offset);
	return a;
}

Allocation GpuAllocator::allocate(const vk::MemoryRequirements& req, vk::MemoryPropertyFlags required,
		vk::MemoryPropertyFlags preferred, vk::MemoryPropertyFlags avoided)
{
	int type = -1;
	int best = 0;
	for (u32 i = 0; i < memoryProperties.memoryTypeCount; i++)
	{
		const vk::MemoryPropertyFlags flags = memoryProperties.memoryTypes[i].propertyFlags;
		if (!(req.memoryTypeBits & (1u << i)) || (flags & required) != required)
			continue;
		const int score = __builtin_popcount((u32)(flags & preferred)) - __builtin_popcount((u32)(flags & avoided));
		if (type < 0 || score > best)
		{
			type = i;
			best = score;
		}
	}
	if (type < 0)
		throw vk::OutOfDeviceMemoryError("GpuAllocator: no memory type for " + vk::to_string(required));
	const vk::MemoryPropertyFlags flags = memoryProperties.memoryTypes[type].propertyFlags;
	// buffers and optimal images share blocks: apart by the granularity (1 on the Brick)
	const vk::DeviceSize alignment = std::max(req.alignment, granularity);
	const vk::DeviceSize size = alignUp(req.size, granularity);

	std::lock_guard<std::mutex> lock(mutex);
	if (size <= DedicatedSize)
		for (auto& block : blocks)
		{
			if (block->dedicated || block->type != (u32)type)
				continue;
			for (auto it = block->free.begin(); it != block->free.end(); ++it)
			{
				const vk::DeviceSize start = alignUp(it->first, alignment);
				const vk::DeviceSize end = it->first + it->second;
				if (start + size > end)
					continue;
				const vk::DeviceSize before = start - it->first;
				block->free.erase(it);
				if (before != 0)
					block->free[start - before] = before;
				if (start + size < end)
					block->free[start + size] = end - start - size;
				block->used++;
				return Allocation(this, block.get(), start, size);
			}
		}

	auto block = std::make_unique<Allocation::Block>();
	block->type = type;
	block->dedicated = size > DedicatedSize;
	block->size = block->dedicated ? size : BlockSize;
	block->coherent = (bool)(flags & vk::MemoryPropertyFlagBits::eHostCoherent);
	block->memory = device.allocateMemory(vk::MemoryAllocateInfo(block->size, type));
	if (flags & vk::MemoryPropertyFlagBits::eHostVisible)
		block->mapped = (u8 *)device.mapMemory(block->memory, 0, VK_WHOLE_SIZE);
	if (block->size > size)
		block->free[size] = block->size - size;
	block->used = 1;
	DEBUG_LOG(RENDERER, "GpuAllocator: %s block of %d KB, type %d", block->dedicated ? "dedicated" : "new", (int)(block->size >> 10), type);
	blocks.push_back(std::move(block));
	return Allocation(this, blocks.back().get(), 0, size);
}

void GpuAllocator::free(Allocation::Block *block, vk::DeviceSize offset, vk::DeviceSize size)
{
	std::lock_guard<std::mutex> lock(mutex);
	if (--block->used == 0 && (block->dedicated || std::count_if(blocks.begin(), blocks.end(),
			[block](const auto& b) { return !b->dedicated && b->type == block->type; }) > 1))
	{
		// the last of a dedicated block, or of a block of a type with others: given back
		if (block->mapped != nullptr)
			device.unmapMemory(block->memory);
		device.freeMemory(block->memory);
		blocks.erase(std::find_if(blocks.begin(), blocks.end(), [block](const auto& b) { return b.get() == block; }));
		return;
	}
	auto next = block->free.lower_bound(offset);
	if (next != block->free.end() && next->first == offset + size)
	{
		size += next->second;
		next = block->free.erase(next);
	}
	if (next != block->free.begin())
	{
		auto prev = std::prev(next);
		if (prev->first + prev->second == offset)
		{
			prev->second += size;
			return;
		}
	}
	block->free[offset] = size;
}
