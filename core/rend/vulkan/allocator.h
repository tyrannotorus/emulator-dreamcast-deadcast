// Deadcast: the Vulkan renderer's device memory. The Brick's GPU (PowerVR GE8300) shares one heap with the
// CPU, whose memory types differ only in being host visible, cached or lazily allocated: buffers and
// images are placed in blocks of one type (32 MB, kept mapped when host visible), first fit, and
// anything larger than a quarter of a block gets memory of its own.
#pragma once
#include "vulkan.h"

#include <map>
#include <memory>
#include <mutex>
#include <vector>

class GpuAllocator;

class Allocation
{
public:
	Allocation() = default;
	Allocation(const Allocation& other) = delete;
	Allocation& operator=(const Allocation& other) = delete;
	Allocation(Allocation&& other) noexcept { *this = std::move(other); }
	Allocation& operator=(Allocation&& other) noexcept
	{
		std::swap(allocator, other.allocator);
		std::swap(block, other.block);
		std::swap(offset, other.offset);
		std::swap(size, other.size);
		return *this;
	}
	~Allocation();

	bool IsHostVisible() const;
	// the memory, mapped (host visible memory only)
	void *MapMemory() const;
	void UnmapMemory() const;

private:
	struct Block
	{
		vk::DeviceMemory memory;
		u32 type = 0;
		vk::DeviceSize size = 0;
		bool dedicated = false;
		bool coherent = true;
		u8 *mapped = nullptr;
		std::map<vk::DeviceSize, vk::DeviceSize> free;	// offset -> size, never adjacent
		u32 used = 0;									// allocations in it
	};
	Allocation(GpuAllocator *allocator, Block *block, vk::DeviceSize offset, vk::DeviceSize size)
		: allocator(allocator), block(block), offset(offset), size(size) {}

	GpuAllocator *allocator = nullptr;
	Block *block = nullptr;
	vk::DeviceSize offset = 0;
	vk::DeviceSize size = 0;

	friend class GpuAllocator;
};

class GpuAllocator
{
public:
	void Init(vk::PhysicalDevice physicalDevice, vk::Device device);
	void Term();

	// memory for the image or buffer, bound to it: of a type with all the required flags, the most of
	// the preferred ones and the fewest avoided
	Allocation AllocateForImage(vk::Image image, vk::MemoryPropertyFlags required,
			vk::MemoryPropertyFlags preferred = {}, vk::MemoryPropertyFlags avoided = {});
	Allocation AllocateForBuffer(vk::Buffer buffer, vk::MemoryPropertyFlags required,
			vk::MemoryPropertyFlags preferred = {}, vk::MemoryPropertyFlags avoided = {});

private:
	Allocation allocate(const vk::MemoryRequirements& req, vk::MemoryPropertyFlags required,
			vk::MemoryPropertyFlags preferred, vk::MemoryPropertyFlags avoided);
	void free(Allocation::Block *block, vk::DeviceSize offset, vk::DeviceSize size);

	vk::Device device;
	vk::PhysicalDeviceMemoryProperties memoryProperties;
	vk::DeviceSize granularity = 1;
	std::mutex mutex;
	std::vector<std::unique_ptr<Allocation::Block>> blocks;

	friend class Allocation;
};
