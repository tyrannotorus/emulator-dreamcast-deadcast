/*
    Created on: Oct 2, 2019

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
#include "vulkan_context.h"
#include "vulkan_renderer.h"
#include "ui/gui.h"
#include "utils.h"
#include "emulator.h"
#include "oslib/oslib.h"
#include "oslib/directory.h"
#include "rend/transform_matrix.h"

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

#include <memory>
#include <set>
#include <algorithm>
#include <cstring>

void ReInitOSD();

// Deadcast: the Brick's GPU (PowerVR GE8300) through the device's Vulkan loader, its frames to the
// screen through fb0 (panel.h): no window system, surface or swapchain.
bool VulkanContext::InitInstance()
{
	try
	{
		static vk::detail::DynamicLoader dl;
		PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = dl.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
		if (vkGetInstanceProcAddr == nullptr) {
			ERROR_LOG(RENDERER, "Vulkan entry point vkGetInstanceProcAddr not found");
			return false;
		}
		VULKAN_HPP_DEFAULT_DISPATCHER.init(vkGetInstanceProcAddr);

		vk::ApplicationInfo applicationInfo("Deadcast", 1, "Deadcast", 1, VK_API_VERSION_1_1);
		// no surface is made (panel.h)
		instance = vk::createInstanceUnique(vk::InstanceCreateInfo(vk::InstanceCreateFlags(), &applicationInfo));
		VULKAN_HPP_DEFAULT_DISPATCHER.init(*instance);

		auto devices = instance->enumeratePhysicalDevices();
		if (devices.empty())
		{
			ERROR_LOG(RENDERER, "Vulkan error: no physical devices found");
			return false;
		}
		physicalDevice = devices.front();

		const auto properties2 = physicalDevice.getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceMaintenance3Properties>();
		const vk::PhysicalDeviceProperties& properties = properties2.get<vk::PhysicalDeviceProperties2>().properties;
		maxMemoryAllocationSize = properties2.get<vk::PhysicalDeviceMaintenance3Properties>().maxMemoryAllocationSize;
		if (maxMemoryAllocationSize == 0)
			maxMemoryAllocationSize = 0xFFFFFFFFu;
		uniformBufferAlignment = properties.limits.minUniformBufferOffsetAlignment;
		storageBufferAlignment = properties.limits.minStorageBufferOffsetAlignment;
		maxStorageBufferRange = properties.limits.maxStorageBufferRange;
		maxSamplerAnisotropy =  properties.limits.maxSamplerAnisotropy;
		vendorID = properties.vendorID;
		NOTICE_LOG(RENDERER, "Vulkan API %d.%d.%d. Device %s", VK_API_VERSION_MAJOR(properties.apiVersion), VK_API_VERSION_MINOR(properties.apiVersion),
				VK_API_VERSION_PATCH(properties.apiVersion), properties.deviceName.data());

		const auto optimalSupported = [this](vk::Format format) {
			const vk::FormatProperties formatProperties = physicalDevice.getFormatProperties(format);
			return (formatProperties.optimalTilingFeatures & vk::FormatFeatureFlagBits::eSampledImage)
					&& (formatProperties.optimalTilingFeatures & vk::FormatFeatureFlagBits::eBlitDst)
					&& (formatProperties.optimalTilingFeatures & vk::FormatFeatureFlagBits::eBlitSrc);
		};
		optimalTilingSupported1555 = optimalSupported(vk::Format::eR5G5B5A1UnormPack16);
		optimalTilingSupported565 = optimalSupported(vk::Format::eR5G6B5UnormPack16);
		optimalTilingSupported4444 = optimalSupported(vk::Format::eR4G4B4A4UnormPack16);

		return true;
	}
	catch (const vk::SystemError& err)
	{
		ERROR_LOG(RENDERER, "Vulkan error: %s", err.what());
	}
	catch (const std::exception& err)
	{
		ERROR_LOG(RENDERER, "Vulkan instance init failed: %s", err.what());
	}
	return false;
}

bool VulkanContext::InitDevice()
{
	if (!instance)
		return false;
	try
	{
		std::vector<vk::QueueFamilyProperties> queueFamilyProperties = physicalDevice.getQueueFamilyProperties();
		graphicsQueueIndex = (u32)std::distance(queueFamilyProperties.begin(),
				std::find_if(queueFamilyProperties.begin(), queueFamilyProperties.end(),
						[](vk::QueueFamilyProperties const& qfp) { return qfp.queueFlags & vk::QueueFlagBits::eGraphics; }));
		if (graphicsQueueIndex == queueFamilyProperties.size()) {
			ERROR_LOG(RENDERER, "Could not find a graphics queue");
			return false;
		}

		std::set<std::string> supportedExtensions;
		for (const auto& property : physicalDevice.enumerateDeviceExtensionProperties())
			supportedExtensions.insert(property.extensionName);
		std::vector<const char*> enabledExtensions;
		const auto tryAddDeviceExtension = [&supportedExtensions, &enabledExtensions](std::string_view extensionName) -> bool
		{
			if (supportedExtensions.count(extensionName.data()))
			{
				enabledExtensions.push_back(extensionName.data());
				NOTICE_LOG(RENDERER, "Device extension enabled: %s", extensionName.data());
				return true;
			}
			NOTICE_LOG(RENDERER, "Device extension unavailable: %s", extensionName.data());
			return false;
		};
		// fb0's memory, imported (panel.h)
		if (!tryAddDeviceExtension(vk::KHRExternalMemoryFdExtensionName) || !tryAddDeviceExtension(vk::EXTExternalMemoryDmaBufExtensionName))
			return false;
		provokingVertexSupported = tryAddDeviceExtension(vk::EXTProvokingVertexExtensionName);

		vk::StructureChain<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceProvokingVertexFeaturesEXT> featuresChainHelper;
		vk::PhysicalDeviceFeatures2& featuresChain = featuresChainHelper.get();
		auto& provokingVertexFeatures = featuresChainHelper.get<vk::PhysicalDeviceProvokingVertexFeaturesEXT>();
		if (!provokingVertexSupported)
			featuresChainHelper.unlink<vk::PhysicalDeviceProvokingVertexFeaturesEXT>();
		physicalDevice.getFeatures2(&featuresChain);
		const vk::PhysicalDeviceFeatures& features = featuresChain.features;
		if (provokingVertexSupported)
		{
			provokingVertexSupported &= provokingVertexFeatures.provokingVertexLast;
			NOTICE_LOG(RENDERER, "provokingVertexSupported %d", provokingVertexSupported);
		}
		samplerAnisotropy = features.samplerAnisotropy;
		fragmentStoresAndAtomics = features.fragmentStoresAndAtomics;

		float queuePriority = 1.0f;
		vk::DeviceQueueCreateInfo deviceQueueCreateInfo(vk::DeviceQueueCreateFlags(), graphicsQueueIndex, 1, &queuePriority);
		vk::DeviceCreateInfo deviceCreateInfo(vk::DeviceCreateFlags(), deviceQueueCreateInfo, nullptr, enabledExtensions);
		deviceCreateInfo.pNext = &featuresChain;
		device = physicalDevice.createDeviceUnique(deviceCreateInfo);
		VULKAN_HPP_DEFAULT_DISPATCHER.init(*device);

		graphicsQueue = device->getQueue(graphicsQueueIndex, 0);

		std::string error;
		if (!panel.init(physicalDevice, *device, error))
		{
			ERROR_LOG(RENDERER, "Vulkan: the screen: %s", error.c_str());
			return false;
		}
		settings.display.refreshRate = panel.refreshRate();

	    // Descriptor pool
        std::array<vk::DescriptorPoolSize, 2> pool_sizes =
        {
            vk::DescriptorPoolSize(vk::DescriptorType::eCombinedImageSampler, 40000),
            vk::DescriptorPoolSize(vk::DescriptorType::eUniformBuffer, 80000),
        };
	    descriptorPool = device->createDescriptorPoolUnique(vk::DescriptorPoolCreateInfo(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet,
	    		40000, pool_sizes));

	    constexpr const char *PipelineSection = "validate";
	    constexpr const char *PipelineKey = "vkPipelineCache";

	    std::string cachePath = hostfs::getShaderCachePath("vulkan_pipeline.cache");
	    bool pipelineCacheCorrupted = config::loadBool(PipelineSection, PipelineKey, false);
	    if (pipelineCacheCorrupted) {
	    	flycast::unlink(cachePath.c_str());
	    	config::saveBool(PipelineSection, PipelineKey, false);
	    }
	    FILE *f = nowide::fopen(cachePath.c_str(), "rb");
	    if (f == nullptr)
	    	pipelineCache = device->createPipelineCacheUnique(vk::PipelineCacheCreateInfo());
	    else
	    {
	    	std::fseek(f, 0, SEEK_END);
	    	size_t cacheSize = std::ftell(f);
	    	std::fseek(f, 0, SEEK_SET);
	    	u8 *cacheData = new u8[cacheSize];
	    	if (std::fread(cacheData, 1, cacheSize, f) != cacheSize)
	    		cacheSize = 0;
	    	std::fclose(f);
    		// if this is still true in the next init, it means the cache file is corrupted
    		config::saveBool(PipelineSection, PipelineKey, true);
	    	try {
	    		pipelineCache = device->createPipelineCacheUnique(vk::PipelineCacheCreateInfo(vk::PipelineCacheCreateFlags(), cacheSize, cacheData));
	    		INFO_LOG(RENDERER, "Vulkan pipeline cache loaded from %s: %zd bytes", cachePath.c_str(), cacheSize);
	    	}
	    	catch (const vk::SystemError& err) {
	    		WARN_LOG(RENDERER, "Error loading pipeline cache: %s", err.what());
	    		pipelineCache = device->createPipelineCacheUnique(vk::PipelineCacheCreateInfo());
	    		flycast::unlink(cachePath.c_str());
	    	}
    		delete [] cacheData;
    		config::saveBool(PipelineSection, PipelineKey, false);
	    }
	    allocator.Init(physicalDevice, *device);

	    shaderManager = std::make_unique<ShaderManager>();
	    quadPipeline = std::make_unique<QuadPipeline>(true, false);
	    quadPipelineWithAlpha = std::make_unique<QuadPipeline>(false, false);
	    quadDrawer = std::make_unique<QuadDrawer>();
	    quadRotatePipeline = std::make_unique<QuadPipeline>(true, true);
	    quadRotateDrawer = std::make_unique<QuadDrawer>();

		vk::PhysicalDeviceProperties props = physicalDevice.getProperties();
		driverName = (const char *)props.deviceName;
		driverVersion = std::to_string(VK_API_VERSION_MAJOR(props.driverVersion)) + "."
				+ std::to_string(VK_API_VERSION_MINOR(props.driverVersion)) + "."
				+ std::to_string(VK_API_VERSION_PATCH(props.driverVersion));

		CreateSwapChain();

		return true;
	}
	catch (const vk::SystemError& err)
	{
		ERROR_LOG(RENDERER, "Vulkan error: %s", err.what());
	}
	catch (const InvalidVulkanContext&)
	{
	}
	return false;
}

// Deadcast: the frames' targets: the panel's pages (panel.h), each with its framebuffer, command pool and fence
void VulkanContext::CreateSwapChain()
{
	try
	{
		device->waitIdle();
		panel.drain();
		inFlightObjects.clear();
		overlay->Term();
		framebuffers.clear();
		drawFences.clear();
		commandBuffers.clear();
		commandPools.clear();
		rendering = false;
		renderDone = false;

		const vk::Extent2D extent = panel.extent();
		SetWindowSize(extent.width, extent.height);
		resized = false;

		for (u32 i = 0; i < Panel::Pages; i++)
		{
			commandPools.push_back(device->createCommandPoolUnique(vk::CommandPoolCreateInfo(vk::CommandPoolCreateFlagBits::eTransient, graphicsQueueIndex)));
			commandBuffers.push_back(std::move(device->allocateCommandBuffersUnique(vk::CommandBufferAllocateInfo(*commandPools.back(), vk::CommandBufferLevel::ePrimary, 1)).front()));
		}

	    depthFormat = findDepthFormat(physicalDevice);
		if (depthFormat == vk::Format::eUndefined) {
			SetWindowSize(0, 0);
			throw InvalidVulkanContext();
		}

	    // Render pass: into the page, written out for the display engine to read once its fence has signalled
	    vk::AttachmentDescription attachmentDescription = vk::AttachmentDescription(vk::AttachmentDescriptionFlags(), Panel::format(), vk::SampleCountFlagBits::e1,
	    		vk::AttachmentLoadOp::eClear, vk::AttachmentStoreOp::eStore, vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eDontCare,
				vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral);
	    vk::AttachmentReference colorReference(0, vk::ImageLayout::eColorAttachmentOptimal);
	    vk::SubpassDescription subpass(vk::SubpassDescriptionFlags(), vk::PipelineBindPoint::eGraphics, nullptr, colorReference,
	    		nullptr, nullptr);
	    vk::SubpassDependency dependency(0, vk::SubpassExternal, vk::PipelineStageFlagBits::eColorAttachmentOutput, vk::PipelineStageFlagBits::eHost,
	    		vk::AccessFlagBits::eColorAttachmentWrite, vk::AccessFlagBits::eHostRead);
	    renderPass = device->createRenderPassUnique(vk::RenderPassCreateInfo(vk::RenderPassCreateFlags(),
	    		attachmentDescription, subpass, dependency));

	    for (u32 i = 0; i < Panel::Pages; i++)
	    {
	    	vk::ImageView view = panel.view(i);
	    	framebuffers.push_back(device->createFramebufferUnique(vk::FramebufferCreateInfo(vk::FramebufferCreateFlags(), *renderPass,
	    			view, width, height, 1)));
	    	drawFences.push_back(device->createFenceUnique(vk::FenceCreateInfo(vk::FenceCreateFlagBits::eSignaled)));
	    }
	    inFlightObjects.resize(Panel::Pages);
	    pageTimer.init(physicalDevice, *device, graphicsQueueIndex, Panel::Pages, 1);
	    quadPipeline->Init(shaderManager.get(), *renderPass, 0);
	    quadPipelineWithAlpha->Init(shaderManager.get(), *renderPass, 0);
	    quadDrawer->Init(quadPipeline.get());
	    quadRotatePipeline->Init(shaderManager.get(), *renderPass, 0);
	    quadRotateDrawer->Init(quadRotatePipeline.get());
	    overlay->Init(quadPipelineWithAlpha.get());

	    currentImage = GetSwapChainSize() - 1;
	    ReInitOSD();

	    INFO_LOG(RENDERER, "Vulkan frames: %d x %d, %d pages", width, height, Panel::Pages);
	}
	catch (const vk::SystemError& err)
	{
		ERROR_LOG(RENDERER, "Vulkan error: %s", err.what());
		SetWindowSize(0, 0);
		throw InvalidVulkanContext();
	}
}

bool VulkanContext::init()
{
	if (!InitInstance()) {
		term();
		return false;
	}
	settings.display.dpi = 96.f;
	settings.display.uiScale = 1.f;
	overlay = std::make_unique<VulkanOverlay>();

	if (!InitDevice()) {
		term();
		return false;
	}

	return true;
}

bool VulkanContext::recreateSwapChainIfNeeded()
{
	if (resized)
	{
		CreateSwapChain();
		lastFrameView = vk::ImageView();
		return true;
	}
	else
		return false;
}

void VulkanContext::NewFrame()
{
	recreateSwapChainIfNeeded();
	if (!IsValid())
		throw InvalidVulkanContext();
	currentImage = (currentImage + 1) % GetSwapChainSize();
	const unsigned long long waitStart = pageTimer.active() ? perfNow() : 0;
	perfTrace('n');
	panel.waitWritable(currentImage);
	try {
		vk::Result res = device->waitForFences(*drawFences[currentImage], true, UINT64_MAX);
		if (res != vk::Result::eSuccess)
			throw InvalidVulkanContext();
	} catch (const vk::SystemError& e) {
		WARN_LOG(RENDERER, "vk:SystemError: %s", e.what());
		throw FlycastException("Vulkan system error");
	}
	perfTrace('N');
	if (pageTimer.active())
	{
		perfRenderBlocked(1, waitStart);
		pageTimer.collect(currentImage);
	}
	device->resetCommandPool(*commandPools[currentImage], vk::CommandPoolResetFlagBits::eReleaseResources);
	inFlightObjects[currentImage].clear();
	vk::CommandBuffer commandBuffer = *commandBuffers[currentImage];
	commandBuffer.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
	if (pageTimer.active())
		pageTimer.start(commandBuffer, currentImage);
	assert(!rendering);
	rendering = true;
}

void VulkanContext::BeginRenderPass()
{
	if (!IsValid())
		return;
	const std::array<vk::ClearValue, 2> clear_colors = { getBorderColor(), vk::ClearDepthStencilValue{ 0.f, 0 } };
	vk::CommandBuffer commandBuffer = *commandBuffers[currentImage];
	commandBuffer.beginRenderPass(vk::RenderPassBeginInfo(*renderPass, *framebuffers[currentImage], vk::Rect2D({0, 0}, {width, height}), clear_colors),
			vk::SubpassContents::eInline);
}

void VulkanContext::EndFrame(vk::CommandBuffer overlayCmdBuffer)
{
	if (!IsValid())
		return;
	vk::CommandBuffer commandBuffer = *commandBuffers[currentImage];
	commandBuffer.endRenderPass();
	if (pageTimer.active())
		pageTimer.stop(commandBuffer, currentImage);
	commandBuffer.end();
	std::vector<vk::CommandBuffer> allCmdBuffers;
	if (overlayCmdBuffer)
		allCmdBuffers.push_back(overlayCmdBuffer);
	allCmdBuffers.push_back(commandBuffer);
	device->resetFences(*drawFences[currentImage]);
	const unsigned long long submitStart = perfNow();
	graphicsQueue.submit(vk::SubmitInfo(nullptr, nullptr, allCmdBuffers), *drawFences[currentImage]);
	perfSubmitted(1, submitStart);
	perfTrace('E');
	assert(rendering);
	rendering = false;
	renderDone = true;
}

void VulkanContext::present()
{
	try {
		Present();
	} catch (const vk::SystemError& e) {
		ERROR_LOG(RENDERER, "Vulkan system error: %s", e.what());
		throw FlycastException("Vulkan system error");
	}
}

void VulkanContext::Present() noexcept
{
	if (renderDone)
	{
		panel.queue(currentImage, *drawFences[currentImage]);
		renderDone = false;
	}
	if (resized)
		try {
			CreateSwapChain();
			lastFrameView = vk::ImageView();
		} catch (const InvalidVulkanContext&) {
		}
}

void VulkanContext::DrawFrame(vk::ImageView imageView, const vk::Extent2D& extent, float aspectRatio)
{
	QuadVertex vtx[4] {
		{ -1, -1, 0, 0, 0 },
		{  1, -1, 0, 1, 0 },
		{ -1,  1, 0, 0, 1 },
		{  1,  1, 0, 1, 1 },
	};
	float shiftX, shiftY;
	getVideoShift(shiftX, shiftY);
	vtx[0].x = vtx[2].x = -1.f + shiftX * 2.f / extent.width;
	vtx[1].x = vtx[3].x = vtx[0].x + 2;
	vtx[0].y = vtx[1].y = -1.f + shiftY * 2.f / extent.height;
	vtx[2].y = vtx[3].y = vtx[0].y + 2;

	vk::CommandBuffer commandBuffer = GetCurrentCommandBuffer();

	static const float scopeColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	CommandBufferDebugScope _(commandBuffer, "DrawFrame", scopeColor);

	if (config::Rotate90)
		quadRotatePipeline->BindPipeline(commandBuffer);
	else
		quadPipeline->BindPipeline(commandBuffer);

	int dx = 0;
	int dy = 0;
	getWindowboxDimensions(width, height, aspectRatio, dx, dy, config::Rotate90);
	
	vk::Viewport viewport(dx, dy, width - dx * 2, height - dy * 2);
	commandBuffer.setViewport(0, viewport);
	commandBuffer.setScissor(0, vk::Rect2D(vk::Offset2D(dx, dy), vk::Extent2D(width - dx * 2, height - dy * 2)));
	if (config::Rotate90)
		quadRotateDrawer->Draw(commandBuffer, imageView, vtx, !config::LinearInterpolation);
	else
		quadDrawer->Draw(commandBuffer, imageView, vtx, !config::LinearInterpolation);
}

void VulkanContext::WaitIdle() const
{
	try {
		graphicsQueue.waitIdle();
	} catch (const vk::Error &err) {
		WARN_LOG(RENDERER, "WaitIdle: %s", err.what());
	}
}

vk::CommandBuffer VulkanContext::PrepareOverlay(bool vmu, bool crosshair)
{
	return overlay->Prepare(*commandPools[GetCurrentImageIndex()], vmu, crosshair);
}

 void VulkanContext::DrawOverlay(float scaling, bool vmu, bool crosshair)
{
	 if (IsValid())
		 overlay->Draw(GetCurrentCommandBuffer(), vk::Extent2D(width, height), scaling, vmu, crosshair);
}

extern Renderer *renderer;

void VulkanContext::PresentFrame(vk::Image image, vk::ImageView imageView, const vk::Extent2D& extent, float aspectRatio) noexcept
{
	lastFrameView = imageView;
	lastFrameExtent = extent;
	lastFrameAR = aspectRatio;

	if (imageView && IsValid())
	{
		try {
			NewFrame();
			auto overlayCmdBuffer = PrepareOverlay(config::FloatVMUs, true);
			BeginRenderPass();

			if (lastFrameView) // Might have been nullified if swap chain recreated
				DrawFrame(imageView, extent, aspectRatio);

			DrawOverlay(settings.display.uiScale, config::FloatVMUs, true);
			EndFrame(overlayCmdBuffer);

		} catch (const InvalidVulkanContext&) {
			// Re-create swap chain
			resized = true;
		}
	}
}

bool VulkanContext::showLastFrameAgain()
{
	if (renderer == nullptr || !renderer->RenderLastFrame() || !lastFrameView || !IsValid())
		return false;
	try {
		NewFrame();
		BeginRenderPass();
		DrawFrame(lastFrameView, lastFrameExtent, lastFrameAR);
		EndFrame();
		return true;
	} catch (const InvalidVulkanContext&) {
		resized = true;
		return false;
	}
}

void VulkanContext::PresentLastFrame()
{
	if (lastFrameView && IsValid())
		DrawFrame(lastFrameView, lastFrameExtent, lastFrameAR);
}

void VulkanContext::term()
{
	pageTimer.term();
	lastFrameView = nullptr;
	if (device && graphicsQueue)
		WaitIdle();
	panel.drain();
	if (device && !drawFences.empty())
	{
		std::vector<vk::Fence> allFences = vk::uniqueToRaw(drawFences);
		try {
			vk::Result res = device->waitForFences(allFences, true, UINT64_MAX);
			if (res != vk::Result::eSuccess)
				INFO_LOG(RENDERER, "VulkanContext::term: waitForFences failed %d", (int)res);
		} catch (const vk::SystemError& e) {
		}
	}
	inFlightObjects.clear();
	if (device && pipelineCache)
	{
		std::vector<u8> cacheData = device->getPipelineCacheData(*pipelineCache);
		if (!cacheData.empty())
		{
			std::string cachePath = hostfs::getShaderCachePath("vulkan_pipeline.cache");
			FILE *f = nowide::fopen(cachePath.c_str(), "wb");
			if (f != nullptr)
			{
				(void)std::fwrite(&cacheData[0], 1, cacheData.size(), f);
				std::fclose(f);
			}
		}
	}
	overlay.reset();
	framebuffers.clear();
	renderPass.reset();
	quadDrawer.reset();
	quadPipeline.reset();
	quadPipelineWithAlpha.reset();
	quadRotateDrawer.reset();
	quadRotatePipeline.reset();
	shaderManager.reset();
	descriptorPool.reset();
	commandBuffers.clear();
	commandPools.clear();
	drawFences.clear();
	panel.term();
	allocator.Term();
	pipelineCache.reset();
	device.reset();
	instance.reset();
}

void VulkanContext::SetWindowSize(u32 width, u32 height)
{
	if (width != this->width || height != this->height)
	{
		this->width = width;
		this->height = height;
		if (width != 0)
			settings.display.width = width;
		if (height != 0)
			settings.display.height = height;
		resize();
	}
}

void VulkanContext::Create(void *window, void *display) {
	new VulkanContext(window, display);
}

VulkanContext::VulkanContext(void *window, void *display)
	: GraphicsContext(window, display)
{
	if (!init())
		throw FlycastException("Vulkan initialization failed");
}

VulkanContext::~VulkanContext() {
	term();
}

bool VulkanContext::GetLastFrame(std::vector<u8>& data, int& width, int& height)
{
	// Deadcast: what the screen shows, from fb0 (Panel::readShown), at its own size whatever size is
	// asked. Never lastFrameView's image: by then its memory can hold other images (garbled pictures).
	return panel.readShown(data, width, height);
}

// Deadcast: every frame goes to the screen at its vblank (panel.h): no other interval
void VulkanContext::setSwapInterval(int interval)
{
}
