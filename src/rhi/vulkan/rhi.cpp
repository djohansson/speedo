#include "../rhi.h"

#include "utils.h"

#include <core/assert.h>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <memory>
#include <tuple>
#include <utility>
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

	std::sort(
		graphicsDeviceCandidates.begin(),
		graphicsDeviceCandidates.end(),
		[&instance, &physicalDevices](const auto& lhs, const auto& rhs)
		{
			constexpr uint32_t kDeviceTypePriority[]{
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

	SwapchainConfiguration<kVk> config{swapchainInfo.capabilities.currentExtent};

	constexpr Format<kVk> kRequestSurfaceImageFormat[]{
		VK_FORMAT_B8G8R8A8_UNORM,
		VK_FORMAT_R8G8B8A8_UNORM,
		VK_FORMAT_B8G8R8_UNORM,
		VK_FORMAT_R8G8B8_UNORM};
	constexpr ColorSpace<kVk> kRequestSurfaceColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	constexpr PresentMode<kVk> kRequestPresentMode[]{
		VK_PRESENT_MODE_MAILBOX_KHR,
		VK_PRESENT_MODE_FIFO_RELAXED_KHR,
		VK_PRESENT_MODE_FIFO_KHR,
		VK_PRESENT_MODE_IMMEDIATE_KHR};

	// Request several formats, the first found will be used
	// If none of the requested image formats could be found, use the first available
	for (auto requestIt : kRequestSurfaceImageFormat)
	{
		SurfaceFormat<kVk> requestedFormat{requestIt, kRequestSurfaceColorSpace};

		auto formatIt = std::find_if(
			swapchainInfo.formats.begin(),
			swapchainInfo.formats.end(),
			[&requestedFormat](VkSurfaceFormatKHR format)
			{
				return requestedFormat.format == format.format &&
						requestedFormat.colorSpace == format.colorSpace;
			});

		if (formatIt != swapchainInfo.formats.end())
		{
			config.surfaceFormat = *formatIt;
			break;
		}
	}

	// Request a certain mode and confirm that it is available. If not use
	// VK_PRESENT_MODE_FIFO_KHR which is mandatory
	for (auto requestIt : kRequestPresentMode)
	{
		auto modeIt = std::find(
			swapchainInfo.presentModes.begin(), swapchainInfo.presentModes.end(), requestIt);

		if (modeIt != swapchainInfo.presentModes.end())
		{
			config.presentMode = *modeIt;

			switch (config.presentMode)
			{
			case VK_PRESENT_MODE_MAILBOX_KHR:
				config.imageCount = 3;
				break;
			default:
				config.imageCount = 2;
				break;
			}

			break;
		}
	}

	return config;
};

void CreateQueues(RHI<kVk>& rhi)
{
	ZoneScopedN("rhiapplication::createQueues");

	auto& queues = rhi.queues;

	VkCommandPoolCreateFlags cmdPoolCreateFlags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;

	constexpr bool kUsePoolReset = true;
	if (kUsePoolReset)
		cmdPoolCreateFlags |= VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

	static constexpr unsigned minGraphicsQueueCount = 2;
	static constexpr unsigned minComputeQueueCount = 1;
	static constexpr unsigned minTransferQueueCount = 1;

	auto [graphicsQueuesIt, graphicsQueuesWasInserted] = queues.emplace(
		kQueueTypeGraphics,
		std::make_tuple(Semaphore<kVk>{rhi.device, SemaphoreCreateDesc<kVk>{VK_SEMAPHORE_TYPE_TIMELINE}}, 0U, ConcurrentAccess<std::vector<QueueContext<kVk>>>{}));

	auto [computeQueuesIt, computeQueuesWasInserted] = queues.emplace(
		kQueueTypeCompute,
		std::make_tuple(Semaphore<kVk>{rhi.device, SemaphoreCreateDesc<kVk>{VK_SEMAPHORE_TYPE_TIMELINE}}, 0U, ConcurrentAccess<std::vector<QueueContext<kVk>>>{}));

	auto [transferQueuesIt, transferQueuesWasInserted] = queues.emplace(
		kQueueTypeTransfer,
		std::make_tuple(Semaphore<kVk>{rhi.device, SemaphoreCreateDesc<kVk>{VK_SEMAPHORE_TYPE_TIMELINE}}, 0U, ConcurrentAccess<std::vector<QueueContext<kVk>>>{}));

	auto IsDedicatedQueueFamily = [](const QueueFamilyDesc<kVk>& queueFamily, VkQueueFlagBits type)
	{
		return (queueFamily.flags & type) && (queueFamily.flags >= type) && (queueFamily.queueCount > 0);
	};

	auto& [graphicsSemaphore, graphicsSemaphoreValue, graphicsQueueInfos] = queues[kQueueTypeGraphics];
	auto& [computeSemaphore, computeSemaphoreValue, computeQueueInfos] = queues[kQueueTypeCompute];
	auto& [transferSemaphore, transferSemaphoreValue, transferQueueInfos] = queues[kQueueTypeTransfer];
	auto graphicsQueueInfosWriteScope = ConcurrentWriteScope(graphicsQueueInfos);
	auto computeQueueInfosWriteScope = ConcurrentWriteScope(computeQueueInfos);
	auto transferQueueInfosWriteScope = ConcurrentWriteScope(transferQueueInfos);
	
	const auto& queueFamilies = rhi.device->GetQueueFamilies();
	for (unsigned queueFamilyIt = 0; queueFamilyIt < queueFamilies.size(); queueFamilyIt++)
	{
		const auto& queueFamily = queueFamilies[queueFamilyIt];

		auto queueCount = queueFamily.queueCount;

		if (IsDedicatedQueueFamily(queueFamily, VK_QUEUE_GRAPHICS_BIT))
		{
			for (unsigned queueIt = 0; queueIt < queueCount; queueIt++)
			{
				auto& [queue, syncInfo] = graphicsQueueInfosWriteScope->emplace_back();
				queue = Queue<kVk>(
					rhi.device,
					CommandPoolCreateDesc<kVk>{.flags = cmdPoolCreateFlags, .queueFamilyIndex = queueFamilyIt, .levelCount = 1, .supportsProfiling = 1},
					QueueCreateDesc<kVk>{.queueIndex = queueIt, .queueFamilyIndex = queueFamilyIt});
			}
		}
		else if (IsDedicatedQueueFamily(queueFamily, VK_QUEUE_COMPUTE_BIT))
		{
			for (unsigned queueIt = 0; queueIt < queueCount; queueIt++)
			{
				auto& [queue, syncInfo] = computeQueueInfosWriteScope->emplace_back();
				queue = Queue<kVk>(
					rhi.device,
					CommandPoolCreateDesc<kVk>{.flags = cmdPoolCreateFlags, .queueFamilyIndex = queueFamilyIt, .levelCount = 0, .supportsProfiling = 1},
					QueueCreateDesc<kVk>{.queueIndex = queueIt, .queueFamilyIndex = queueFamilyIt});
			}
		}
		else if (IsDedicatedQueueFamily(queueFamily, VK_QUEUE_TRANSFER_BIT))
		{
			for (unsigned queueIt = 0; queueIt < queueCount; queueIt++)
			{
				auto& [queue, syncInfo] = transferQueueInfosWriteScope->emplace_back();
				queue = Queue<kVk>(
					rhi.device,
					CommandPoolCreateDesc<kVk>{.flags = cmdPoolCreateFlags, .queueFamilyIndex = queueFamilyIt, .levelCount = 0, .supportsProfiling = 0},
					QueueCreateDesc<kVk>{.queueIndex = queueIt, .queueFamilyIndex = queueFamilyIt});
			}
		}
	}

	ENSUREF(!graphicsQueueInfosWriteScope->empty(), "Failed to find a suitable graphics queue!");

	if (computeQueueInfosWriteScope->empty())
	{
		ENSUREF(graphicsQueueInfosWriteScope->size() >= (minComputeQueueCount + minGraphicsQueueCount), "Failed to find a suitable compute queue!");

		computeQueueInfosWriteScope->emplace_back(std::make_pair(
			std::move(graphicsQueueInfosWriteScope->back().first),
			QueueHostSyncInfo<kVk>{}));
		graphicsQueueInfosWriteScope->pop_back();
	}

	if (transferQueueInfosWriteScope->empty())
	{
		ENSUREF(graphicsQueueInfosWriteScope->size() >= (minTransferQueueCount + minGraphicsQueueCount), "Failed to find a suitable transfer queue!");

		transferQueueInfosWriteScope->emplace_back(std::make_pair(
			std::move(graphicsQueueInfosWriteScope->back().first),
			QueueHostSyncInfo<kVk>{}));
		graphicsQueueInfosWriteScope->pop_back();
	}
}

Window<kVk> CreateRHIWindow(
	const std::shared_ptr<Device<kVk>>& device,
	WindowHandle&& window,
	SurfaceHandle<kVk>&& surface,
	typename Window<kVk>::ConfigFile&& windowConfig,
	WindowState&& windowState)
{
	return {
		device,
		std::forward<WindowHandle>(window),
		std::forward<SurfaceHandle<kVk>>(surface),
		std::forward<Window<kVk>::ConfigFile>(windowConfig),
		std::forward<WindowState>(windowState)};
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
	return std::make_shared<Instance<kVk>>(InstanceConfiguration<kVk>{name.data(), "speedo"});
}

template <>
void ConstructWindowDependentObjects(RHI<kVk>& rhi)
{
	ZoneScopedN("rhiapplication::ConstructWindowDependentObjects");
	
	auto& window = rhi.GetWindow(GetCurrentWindow());
	auto frameCount = window.GetFrames().size();
	rhi.renderImageSets.reserve(frameCount);
	for (unsigned frameIt = 0; frameIt < frameCount; frameIt++)
	{
		auto colorImage = std::make_shared<Image<kVk>>(
			rhi.device,
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
			rhi.device,
			ImageCreateDesc<kVk>{
				.mipLevels = {{.extent = window.GetConfig().swapchainConfig.extent}},
				.format = FindSupportedFormat(
					rhi.device->GetPhysicalDevice(),
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

		rhi.renderImageSets.emplace_back(rhi.device, std::vector{colorImage, depthStencilImage});
	}

	{
		auto& [graphicsSemaphore, graphicsSemaphoreValue, graphicsQueueInfos] = rhi.queues[kQueueTypeGraphics];
		auto graphicsQueueInfosWriteScope = ConcurrentWriteScope(graphicsQueueInfos);
		auto& [graphicsQueue, graphicsSubmit] = graphicsQueueInfosWriteScope->front();
		
		auto cmd = graphicsQueue.GetPool().Commands();

		for (auto& frame : rhi.GetWindow(GetCurrentWindow()).GetFrames())
		{
			frame.SetLoadOp(VK_ATTACHMENT_LOAD_OP_CLEAR, 0);
			frame.SetStoreOp(VK_ATTACHMENT_STORE_OP_STORE, 0);
			frame.Transition(cmd, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT, 0);
		}

		for (auto& rt : rhi.renderImageSets)
		{
			rt.SetLoadOp(VK_ATTACHMENT_LOAD_OP_CLEAR, 0);
			rt.SetLoadOp(VK_ATTACHMENT_LOAD_OP_CLEAR, rt.GetAttachments().size() - 1, VK_ATTACHMENT_LOAD_OP_CLEAR);
			rt.SetStoreOp(VK_ATTACHMENT_STORE_OP_STORE, 0);
			rt.SetStoreOp(VK_ATTACHMENT_STORE_OP_STORE, rt.GetAttachments().size() - 1, VK_ATTACHMENT_STORE_OP_STORE);
			rt.Transition(cmd, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT, 0);
			rt.Transition(cmd, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, rt.GetAttachments().size() - 1);
		}

		cmd.End();

		graphicsQueue.EnqueueSubmit(QueueDeviceSyncInfo<kVk>{
			.waitSemaphores = {},
			.waitDstStageMasks = {},
			.waitSemaphoreValues = {},
			.signalSemaphores = {graphicsSemaphore},
			.signalSemaphoreValues = {++graphicsSemaphoreValue}});

		graphicsSubmit = graphicsQueue.Submit();
	}
}

} // namespace detail

template <>
[[nodiscard]] std::unique_ptr<RHIBase> CreateRHI<kVk>(std::string_view name, CreateWindowFunc createWindowFunc)
{
	using namespace detail;

	auto rhiPtr = std::make_unique<RHI<kVk>>();
	auto& rhi = *rhiPtr;
	
	Window<kVk>::ConfigFile windowConfig{
		std::get<std::filesystem::path>(Application::Get().lock()->GetEnv().variables["UserProfilePath"]) / "window.json"};

	WindowState windowState{};
	windowState.width = windowConfig.swapchainConfig.extent.width / windowConfig.contentScale.x;
	windowState.height = windowConfig.swapchainConfig.extent.height / windowConfig.contentScale.y;

	auto windowHandle = createWindowFunc(&windowState);

	auto instance = CreateInstance(name);
	auto surface = CreateSurface(*instance, &instance->GetHostAllocationCallbacks(), windowHandle);
	auto device = CreateDevice(instance, DetectSuitableGraphicsDevice(*instance, surface));
	auto pipeline = CreatePipeline(device);
	
	rhi.instance = std::move(instance);
	rhi.device = std::move(device);
	rhi.pipeline = std::move(pipeline);

	windowConfig.swapchainConfig = DetectSuitableSwapchain(*rhi.device, surface);
	windowConfig.contentScale = {windowState.xscale, windowState.yscale};

	rhi.windows.emplace_back(
		CreateRHIWindow(
			rhi.device,
			std::move(windowHandle),
			std::move(surface),
			std::move(windowConfig),
			std::move(windowState)));

	SetWindows(&windowHandle, 1);
	SetCurrentWindow(windowHandle);

	CreateQueues(rhi);

	ConstructWindowDependentObjects(rhi);

	return rhiPtr;
}

} // namespace rhi
