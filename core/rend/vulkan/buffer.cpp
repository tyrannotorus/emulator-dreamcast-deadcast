/*
 *  Created on: Oct 3, 2019

	Copyright 2019 flyinghead

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
#include "buffer.h"
#include "vulkan_context.h"

BufferData::BufferData(vk::DeviceSize size, vk::BufferUsageFlags usage, vk::MemoryPropertyFlags propertyFlags)
	: bufferSize(size), m_usage(usage)
{
	VulkanContext *context = VulkanContext::Instance();
	buffer = context->GetDevice().createBufferUnique(vk::BufferCreateInfo(vk::BufferCreateFlags(), size, usage));
	if (propertyFlags & vk::MemoryPropertyFlagBits::eDeviceLocal)
		allocation = context->GetAllocator().AllocateForBuffer(*buffer, vk::MemoryPropertyFlagBits::eDeviceLocal);
	else
	{
		// Deadcast: what is read back is cached (the CPU reads uncached memory slowly), else coherent
		vk::MemoryPropertyFlags preferred = (propertyFlags & vk::MemoryPropertyFlagBits::eHostCached)
				? vk::MemoryPropertyFlagBits::eHostCached : propertyFlags & vk::MemoryPropertyFlagBits::eHostCoherent;
		allocation = context->GetAllocator().AllocateForBuffer(*buffer, vk::MemoryPropertyFlagBits::eHostVisible, preferred);
	}
}

BufferPacker::BufferPacker()
{
	uniformAlignment = VulkanContext::Instance()->GetUniformBufferAlignment();
	storageAlignment = VulkanContext::Instance()->GetStorageBufferAlignment();
}
