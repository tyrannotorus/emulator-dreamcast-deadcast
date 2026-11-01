/*
	Copyright 2023 flyinghead

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
#include "commandpool.h"
#include "vulkan_context.h"

void CommandPool::Init(size_t chainSize)
{
	this->chainSize = chainSize;
	device = VulkanContext::Instance()->GetDevice();
	if (commandPools.size() > chainSize)
	{
		commandPools.resize(chainSize);
		fences.resize(chainSize);
	}
	else
	{
		while (commandPools.size() < chainSize)
		{
			commandPools.push_back(device.createCommandPoolUnique(
					vk::CommandPoolCreateInfo(vk::CommandPoolCreateFlagBits::eTransient, VulkanContext::Instance()->GetGraphicsQueueFamilyIndex())));
			fences.push_back(device.createFenceUnique(vk::FenceCreateInfo(vk::FenceCreateFlagBits::eSignaled)));
		}
	}
	freeBuffers.resize(chainSize);
	inFlightBuffers.resize(chainSize);
	inFlightObjects.resize(chainSize);
	if (gpuKind >= 0)
		measureGpu(gpuKind);
}

void CommandPool::measureGpu(int kind)
{
	gpuKind = kind;
	VulkanContext *context = VulkanContext::Instance();
	gpuTimer.init(context->GetPhysicalDevice(), device, context->GetGraphicsQueueFamilyIndex(), (u32)chainSize, kind);
}

void CommandPool::Term()
{
	if (!fences.empty())
	{
		std::vector<vk::Fence> allFences = vk::uniqueToRaw(fences);
		vk::Result res = device.waitForFences(allFences, true, UINT64_MAX);
		if (res != vk::Result::eSuccess)
			WARN_LOG(RENDERER, "CommandPool::Term: waitForFences failed %d", (int)res);
	}
	gpuTimer.term();
	inFlightObjects.clear();
	freeBuffers.clear();
	inFlightBuffers.clear();
	fences.clear();
	commandPools.clear();
}

void CommandPool::BeginFrame()
{
	if (frameStarted)
		return;
	frameStarted = true;
	index = (index + 1) % chainSize;
	const unsigned long long waitStart = gpuKind >= 0 ? perfNow() : 0;
	if (gpuKind >= 0)
		perfTrace('g');
	vk::Result res = device.waitForFences(fences[index].get(), true, UINT64_MAX);
	if (gpuKind >= 0)
		perfTrace('G');
	if (res != vk::Result::eSuccess)
		WARN_LOG(RENDERER, "CommandPool::BeginFrame: waitForFences failed %d", (int)res);
	if (gpuKind >= 0)
	{
		perfRenderBlocked(0, waitStart);
		gpuTimer.collect((u32)index);
	}
	std::vector<vk::UniqueCommandBuffer>& inFlightBuf = inFlightBuffers[index];
	std::vector<vk::UniqueCommandBuffer>& freeBuf = freeBuffers[index];
	std::move(inFlightBuf.begin(), inFlightBuf.end(), std::back_inserter(freeBuf));
	inFlightBuf.clear();
	device.resetCommandPool(*commandPools[index], vk::CommandPoolResetFlagBits::eReleaseResources);
	inFlightObjects[index].clear();
	lastBuffers.clear();
}

void CommandPool::EndFrame()
{
	if (!frameStarted)
		return;
	frameStarted = false;
	std::vector<vk::CommandBuffer> commandBuffers = vk::uniqueToRaw(inFlightBuffers[index]);
	if (!commandBuffers.empty())
	{
		// sort buffers: !last, last
		size_t len = commandBuffers.size() - 1;
		while (len != 0)
		{
			for (size_t i = 0; i < len; i++)
				if (lastBuffers[i] && !lastBuffers[i + 1]) {
					std::vector<bool>::swap(lastBuffers[i], lastBuffers[i + 1]);
					std::swap(commandBuffers[i], commandBuffers[i + 1]);
				}
			len--;
		}
	}
	// Deadcast: measured, two command buffers of its own around the frame's (gputimer.h)
	if (gpuTimer.active() && !commandBuffers.empty())
	{
		// (the order is settled above: Allocate's mark for it no longer matters)
		const vk::CommandBuffer first = Allocate();
		const vk::CommandBuffer last = Allocate();
		const vk::CommandBufferBeginInfo once(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);
		first.begin(once);
		gpuTimer.start(first, (u32)index);
		first.end();
		last.begin(once);
		gpuTimer.stop(last, (u32)index);
		last.end();
		commandBuffers.insert(commandBuffers.begin(), first);
		commandBuffers.push_back(last);
	}
	device.resetFences(fences[index].get());
	VulkanContext::Instance()->SubmitCommandBuffers(commandBuffers, *fences[index]);
}

vk::CommandBuffer CommandPool::Allocate(bool submitLast)
{
	if (freeBuffers[index].empty())
	{
		inFlightBuffers[index].emplace_back(std::move(
				device.allocateCommandBuffersUnique(vk::CommandBufferAllocateInfo(*commandPools[index], vk::CommandBufferLevel::ePrimary, 1))
				.front()));
	}
	else
	{
		inFlightBuffers[index].emplace_back(std::move(freeBuffers[index].back()));
		freeBuffers[index].pop_back();
	}
	lastBuffers.push_back(submitLast);
	return *inFlightBuffers[index].back();
}

void CommandPool::EndFrameAndWait()
{
	EndFrame();
	vk::Result res = device.waitForFences(fences[index].get(), true, UINT64_MAX);
	if (res != vk::Result::eSuccess)
		WARN_LOG(RENDERER, "CommandPool::waitForCommandCompletion: waitForFences failed %d", (int)res);
	inFlightObjects[index].clear();
}
