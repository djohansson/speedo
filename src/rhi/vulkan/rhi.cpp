#include "../rhi.h"
#include "utils.h"

#include <core/assert.h>

#include <cstdint>
#include <iostream>
#include <memory>
#include <tuple>
#include <vector>

namespace rhi
{

namespace detail
{

uint32_t DetectSuitableGraphicsDevice(Instance<kVk>& instance, SurfaceHandle<kVk> surface)
{
	const auto& physicalDevices = instance.GetPhysicalDevices();

	std::vector<std::tuple<uint32_t, uint32_t>> graphicsDeviceCandidates;
	graphicsDeviceCandidates.reserve(physicalDevices.size());

	if constexpr (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
		std::cout << physicalDevices.size() << " vulkan physical device(s) found: " << '\n';

	for (uint32_t physicalDeviceIt = 0; physicalDeviceIt < physicalDevices.size();
			physicalDeviceIt++)
	{
		auto* physicalDevice = physicalDevices[physicalDeviceIt];

		const auto& physicalDeviceInfo = instance.GetPhysicalDeviceInfo(physicalDevice);
		const auto& swapchainInfo = instance.UpdateSwapchainInfo(physicalDevice, surface);

		if constexpr (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
			std::cout << physicalDeviceInfo.deviceProperties.properties.deviceName << '\n';

		for (uint32_t queueFamilyIt = 0;
				queueFamilyIt < physicalDeviceInfo.queueFamilyProperties.size();
				queueFamilyIt++)
		{
			const auto& queueFamilyProperties =
				physicalDeviceInfo.queueFamilyProperties[queueFamilyIt];
			const auto& queueFamilyPresentSupport =
				swapchainInfo.queueFamilyPresentSupport[queueFamilyIt];

			if (((queueFamilyProperties.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0U) &&
				(queueFamilyPresentSupport != 0U))
				graphicsDeviceCandidates.emplace_back(physicalDeviceIt, queueFamilyIt);
		}
	}

	std::ranges::sort(
		graphicsDeviceCandidates,
		[&instance, &physicalDevices](const auto& lhs, const auto& rhs)
		{
			constexpr std::array<uint32_t, 6> kDeviceTypePriority{
				4,		   //VK_PHYSICAL_DEVICE_TYPE_OTHER = 0,
				1,		   //VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU = 1,
				0,		   //VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU = 2,
				2,		   //VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU = 3,
				3,		   //VK_PHYSICAL_DEVICE_TYPE_CPU = 4,
				0x7FFFFFFF //VK_PHYSICAL_DEVICE_TYPE_MAX_ENUM = 0x7FFFFFFF
			};
			const auto& [lhsPhysicalDeviceIndex, lhsQueueFamilyIndex] = lhs;
			const auto& [rhsPhysicalDeviceIndex, rhsQueueFamilyIndex] = rhs;

			auto lhsDeviceType =
				instance.GetPhysicalDeviceInfo(physicalDevices[lhsPhysicalDeviceIndex])
					.deviceProperties.properties.deviceType;
			auto rhsDeviceType =
				instance.GetPhysicalDeviceInfo(physicalDevices[rhsPhysicalDeviceIndex])
					.deviceProperties.properties.deviceType;

			return kDeviceTypePriority[lhsDeviceType] < kDeviceTypePriority[rhsDeviceType];
		});

	ENSUREF(!graphicsDeviceCandidates.empty(), "Failed to find a suitable GPU!");

	return std::get<0>(graphicsDeviceCandidates.front());
}

SwapchainConfiguration<kVk> DetectSuitableSwapchain(Device<kVk>& device, SurfaceHandle<kVk> surface)
{
	const auto& swapchainInfo =
		device.GetInstance()->GetSwapchainInfo(device.GetPhysicalDevice(), surface);

	SwapchainConfiguration<kVk> config{
		.extent = swapchainInfo.capabilities.currentExtent,
		.surfaceFormat = {.format=VK_FORMAT_UNDEFINED, .colorSpace=VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
		.presentMode = VK_PRESENT_MODE_FIFO_KHR,
		.imageCount = static_cast<uint8_t>(swapchainInfo.capabilities.minImageCount),
		.useDynamicRendering = true};

	constexpr std::array<Format<kVk>, 4> kRequestSurfaceImageFormat{
		VK_FORMAT_B8G8R8A8_UNORM,
		VK_FORMAT_R8G8B8A8_UNORM,
		VK_FORMAT_B8G8R8_UNORM,
		VK_FORMAT_R8G8B8_UNORM};
	constexpr ColorSpace<kVk> kRequestSurfaceColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	constexpr std::array<PresentMode<kVk>, 2> kRequestPresentMode{
		VK_PRESENT_MODE_MAILBOX_KHR,
		VK_PRESENT_MODE_FIFO_RELAXED_KHR};

	// Request several formats, the first found will be used
	// If none of the requested image formats could be found, use the first available
	auto formatIt = swapchainInfo.formats.begin();
	for (auto requestIt : kRequestSurfaceImageFormat)
	{
		SurfaceFormat<kVk> requestedFormat{.format=requestIt, .colorSpace=kRequestSurfaceColorSpace};

		formatIt = std::ranges::find_if(
			swapchainInfo.formats,
			[&requestedFormat](VkSurfaceFormatKHR format)
			{
				return requestedFormat.format == format.format &&
						requestedFormat.colorSpace == format.colorSpace;
			});

		if (formatIt != swapchainInfo.formats.end())
			break;
	}
	config.surfaceFormat = *formatIt;

	// Request a certain mode and confirm that it is available. If not use
	// VK_PRESENT_MODE_FIFO_KHR which is mandatory
	for (auto requestIt : kRequestPresentMode)
	{
		auto modeIt = std::ranges::find(
			swapchainInfo.presentModes, requestIt);

		if (modeIt != swapchainInfo.presentModes.end())
		{
			config.presentMode = *modeIt;
			if (config.presentMode == VK_PRESENT_MODE_MAILBOX_KHR)
			{
				ENSURE(swapchainInfo.capabilities.maxImageCount >= 3);
				config.imageCount = 3;
			}
			break;
		}
	}

	return config;
};

void CreateQueues(RHI<kVk>& rhi)
{
	ZoneScopedN("rhiapplication::createQueues");

	auto& queues = rhi.GetQueues();

	VkCommandPoolCreateFlags cmdPoolCreateFlags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;

	constexpr bool kUsePoolReset = true;
	if (kUsePoolReset)
		cmdPoolCreateFlags |= VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

	queues.emplace(
		kQueueTypeGraphics,
		std::make_shared<QueueTimelineContextData<kVk>>(
			Semaphore<kVk>{rhi.GetDevice(), SemaphoreCreateDesc<kVk>{.type=VK_SEMAPHORE_TYPE_TIMELINE}},
			uint64_t{},
			uint32_t{},
			CircularContainer<QueueContext<kVk>>{}));
	queues.emplace(
		kQueueTypeCompute,
		std::make_shared<QueueTimelineContextData<kVk>>(
			Semaphore<kVk>{rhi.GetDevice(), SemaphoreCreateDesc<kVk>{.type=VK_SEMAPHORE_TYPE_TIMELINE}},
			uint64_t{},
			uint32_t{},
			CircularContainer<QueueContext<kVk>>{}));
	queues.emplace(
		kQueueTypeTransfer,
		std::make_shared<QueueTimelineContextData<kVk>>(
			Semaphore<kVk>{rhi.GetDevice(), SemaphoreCreateDesc<kVk>{.type=VK_SEMAPHORE_TYPE_TIMELINE}},
			uint64_t{},
			uint32_t{},
			CircularContainer<QueueContext<kVk>>{}));

	auto isDedicatedQueueFamily = [](const QueueFamilyDesc<kVk>& queueFamily, VkQueueFlagBits type)
	{
		return (queueFamily.flags & type) && (queueFamily.flags >= type) && (queueFamily.queueCount > 0);
	};

	auto graphics = queues[kQueueTypeGraphics].Write();
	auto compute = queues[kQueueTypeCompute].Write();
	auto transfer = queues[kQueueTypeTransfer].Write();
	
	const auto& queueFamilies = rhi.GetDevice()->GetQueueFamilies();
	for (unsigned queueFamilyIt = 0; queueFamilyIt < queueFamilies.size(); queueFamilyIt++)
	{
		const auto& queueFamily = queueFamilies[queueFamilyIt];

		auto queueCount = queueFamily.queueCount;

		if (isDedicatedQueueFamily(queueFamily, VK_QUEUE_GRAPHICS_BIT))
		{
			graphics->queues = std::vector<QueueContext<kVk>>(queueCount);
			graphics->queueFamilyIndex = queueFamilyIt;
			for (unsigned queueIt = 0; queueIt < queueCount; queueIt++)
			{
				auto& [queue, syncInfo] = graphics->queues.FetchAdd();
				queue = Queue<kVk>(
					rhi.GetDevice(),
					CommandPoolCreateDesc<kVk>
					{
						.flags = cmdPoolCreateFlags,
						.queueFamilyIndex = queueFamilyIt,
						.levelCount = 15,
						.supportsProfiling = static_cast<uint32_t>(queueFamily.timestampValidBits > 0)
					},
					QueueCreateDesc<kVk>{.queueIndex = queueIt, .queueFamilyIndex = queueFamilyIt});
			}
		}
		else if (isDedicatedQueueFamily(queueFamily, VK_QUEUE_COMPUTE_BIT))
		{
			compute->queues = std::vector<QueueContext<kVk>>(queueCount);
			compute->queueFamilyIndex = queueFamilyIt;
			for (unsigned queueIt = 0; queueIt < queueCount; queueIt++)
			{
				auto& [queue, syncInfo] = compute->queues.FetchAdd();
				queue = Queue<kVk>(
					rhi.GetDevice(),
					CommandPoolCreateDesc<kVk>
					{
						.flags = cmdPoolCreateFlags,
						.queueFamilyIndex = queueFamilyIt,
						.levelCount = 1,
						.supportsProfiling = static_cast<uint32_t>(queueFamily.timestampValidBits > 0)
					},
					QueueCreateDesc<kVk>{.queueIndex = queueIt, .queueFamilyIndex = queueFamilyIt});
			}
		}
		else if (isDedicatedQueueFamily(queueFamily, VK_QUEUE_TRANSFER_BIT))
		{
			transfer->queues = std::vector<QueueContext<kVk>>(queueCount);
			transfer->queueFamilyIndex = queueFamilyIt;
			for (unsigned queueIt = 0; queueIt < queueCount; queueIt++)
			{
				auto& [queue, syncInfo] = transfer->queues.FetchAdd();
				queue = Queue<kVk>(
					rhi.GetDevice(),
					CommandPoolCreateDesc<kVk>
					{
						.flags = cmdPoolCreateFlags,
						.queueFamilyIndex = queueFamilyIt,
						.levelCount = 1,
						.supportsProfiling = VK_FALSE // requires VK_QUEUE_GRAPHICS_BIT or VK_QUEUE_COMPUTE_BIT
					},
					QueueCreateDesc<kVk>{.queueIndex = queueIt, .queueFamilyIndex = queueFamilyIt});
			}
		}
	}

	ENSUREF(!graphics->queues.Empty(), "Failed to find a suitable graphics queue!");

	if (compute->queues.Empty())
	{
		// Alias compute to graphics queue if no dedicated compute queue is found.
		// This is valid as long as the graphics queue family supports compute operations, which is guaranteed by the Vulkan spec.
		ENSUREF(!graphics->queues.Empty(), "Failed to find a suitable compute queue!");
		compute.Get() = graphics.Get();
	}

	if (transfer->queues.Empty())
	{
		// Alias transfer to compute queue if no dedicated transfer queue is found.
		// This is valid as long as the compute queue family supports transfer operations, which is guaranteed by the Vulkan spec.
		ENSUREF(!compute->queues.Empty(), "Failed to find a suitable transfer queue!");
		transfer.Get() = compute.Get();
	}
}

std::unique_ptr<Pipeline<kVk>> CreatePipeline(const std::shared_ptr<Device<kVk>>& device)
{
	return std::make_unique<Pipeline<kVk>>(
		device,
		PipelineConfiguration<kVk>{(std::get<std::filesystem::path>(Application::Get().lock()->GetEnv().variables["UserProfilePath"]) / "pipeline.cache").string()});
}

std::shared_ptr<Device<kVk>> CreateDevice(
	const std::shared_ptr<Instance<kVk>>& instance,
	uint32_t physicalDeviceIndex)
{
	return std::make_shared<Device<kVk>>(
		instance,
		DeviceConfiguration<kVk>{physicalDeviceIndex});
}

std::shared_ptr<Instance<kVk>> CreateInstance(std::string_view name)
{
	return std::make_shared<Instance<kVk>>(InstanceConfiguration<kVk>{std::string(name), "speedo"});
}

template <>
void ConstructWindowDependentObjects(RHI<kVk>& rhi)
{
	ZoneScopedN("rhiapplication::ConstructWindowDependentObjects");
	
	auto& window = rhi.GetWindow(GetCurrentWindow());
	auto frameCount = window.GetFrames().size();
	rhi.GetResources().renderImageSets.reserve(frameCount);
	for (unsigned frameIt = 0; frameIt < frameCount; frameIt++)
	{
		auto colorImage = std::make_shared<Image<kVk>>(
			rhi.GetDevice(),
			ImageCreateDesc<kVk>{
				.mipLevels = {{.extent = window.GetConfig().swapchainConfig.extent}},
				.format = window.GetConfig().swapchainConfig.surfaceFormat.format,
				.tiling = VK_IMAGE_TILING_OPTIMAL,
				.usageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT/* | VK_IMAGE_USAGE_TRANSFER_DST_BIT*/ | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
				.memoryFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
				.imageAspectFlags = VK_IMAGE_ASPECT_COLOR_BIT,
				.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
				.name = "Main RT Color"});

		auto depthStencilImage = std::make_shared<Image<kVk>>(
			rhi.GetDevice(),
			ImageCreateDesc<kVk>{
				.mipLevels = {{.extent = window.GetConfig().swapchainConfig.extent}},
				.format = FindSupportedFormat(
					rhi.GetDevice()->GetPhysicalDevice(),
					{VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT},
					VK_IMAGE_TILING_OPTIMAL,
					VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
						VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT),
				.tiling = VK_IMAGE_TILING_OPTIMAL,
				.usageFlags = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT/* | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT*/ | VK_IMAGE_USAGE_SAMPLED_BIT,
				.memoryFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
				.imageAspectFlags = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
				.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
				.name = "Main RT DepthStencil"});

		rhi.GetResources().renderImageSets.emplace_back(rhi.GetDevice(), std::vector{colorImage, depthStencilImage});
	}

	{
		auto graphics = rhi.GetQueues()[kQueueTypeGraphics].Write();
		auto& [graphicsQueue, graphicsSubmits] = graphics->queues.Get();
		
		auto cmd = graphicsQueue.GetPool().Commands();

		for (auto& frame : window.GetFrames())
		{
			frame.SetLoadOp(VK_ATTACHMENT_LOAD_OP_CLEAR, 0);
			frame.SetStoreOp(VK_ATTACHMENT_STORE_OP_STORE, 0);
			frame.Transition(cmd, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT, 0);
		}

		for (auto& renderImageSet : rhi.GetResources().renderImageSets)
		{
			renderImageSet.SetLoadOp(VK_ATTACHMENT_LOAD_OP_CLEAR, 0);
			renderImageSet.SetLoadOp(VK_ATTACHMENT_LOAD_OP_CLEAR, renderImageSet.GetAttachments().size() - 1, VK_ATTACHMENT_LOAD_OP_CLEAR);
			renderImageSet.SetStoreOp(VK_ATTACHMENT_STORE_OP_STORE, 0);
			renderImageSet.SetStoreOp(VK_ATTACHMENT_STORE_OP_STORE, renderImageSet.GetAttachments().size() - 1, VK_ATTACHMENT_STORE_OP_STORE);
			renderImageSet.Transition(cmd, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT, 0);
			renderImageSet.Transition(cmd, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, renderImageSet.GetAttachments().size() - 1);
		}

		cmd.End();

		graphicsQueue.EnqueueSubmit(QueueDeviceSyncInfo<kVk>{
			.waitSemaphores = {},
			.waitDstStageMasks = {},
			.waitSemaphoreValues = {},
			.signalSemaphores = {graphics->semaphore},
			.signalSemaphoreValues = {++graphics->timeline}});

		graphicsSubmits |= graphicsQueue.Submit();
	}
}

} // namespace detail

} // namespace rhi

template <>
RHI<kVk>::RHI(std::string_view name, CreateWindowFunc createWindowFunc)
{
	using namespace rhi::detail;

	auto& rhi = *this;
	
	Window<kVk>::ConfigFile windowConfig{
		std::get<std::filesystem::path>(Application::Get().lock()->GetEnv().variables["UserProfilePath"]) / "window.bin"};

	WindowState windowState{};
	windowState.width = static_cast<uint32_t>(static_cast<float>(windowConfig.swapchainConfig.extent.width) / windowConfig.contentScale.x);
	windowState.height = static_cast<uint32_t>(static_cast<float>(windowConfig.swapchainConfig.extent.height) / windowConfig.contentScale.y);

	auto* windowHandle = createWindowFunc(&windowState);

	auto instance = CreateInstance(name);
	auto* surface = CreateSurface(*instance, &instance->GetHostAllocationCallbacks(), windowHandle);
	auto device = CreateDevice(instance, DetectSuitableGraphicsDevice(*instance, surface));
	auto pipeline = CreatePipeline(device);
	
	myInstance = std::move(instance);
	myDevice = std::move(device);
	myPipeline = std::move(pipeline);

	windowConfig.swapchainConfig = DetectSuitableSwapchain(*myDevice, surface);
	windowConfig.contentScale = {windowState.xscale, windowState.yscale};

	if (windowConfig.swapchainConfig.extent.width == ~0U || windowConfig.swapchainConfig.extent.height == ~0U)
		windowConfig.swapchainConfig.extent = {.width = windowState.width, .height = windowState.height};
	
	myWindows.emplace(
		std::make_unique<Window<kVk>>(
			myDevice,
			windowHandle,
			surface,
			std::move(windowConfig),
			std::move(windowState)));

	SetWindows(&windowHandle, 1);
	SetCurrentWindow(windowHandle);

	CreateQueues(*this);
	ConstructWindowDependentObjects(*this);
}
