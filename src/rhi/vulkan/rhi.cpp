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

template <>
uint32_t DetectSuitableGraphicsDevice(Instance<kVk>& instance, SurfaceHandle<kVk> surface)
{
	const auto& physicalDevices = instance.GetPhysicalDevices();

	std::vector<std::tuple<uint32_t, uint32_t>> graphicsDeviceCandidates;
	graphicsDeviceCandidates.reserve(physicalDevices.size());

	if constexpr (GRAPHICS_VALIDATION_LEVEL > 0)
		std::cout << physicalDevices.size() << " vulkan physical device(s) found: " << '\n';

	for (uint32_t physicalDeviceIt = 0; physicalDeviceIt < physicalDevices.size();
			physicalDeviceIt++)
	{
		auto* physicalDevice = physicalDevices[physicalDeviceIt];

		const auto& physicalDeviceInfo = instance.GetPhysicalDeviceInfo(physicalDevice);
		const auto& swapchainInfo = instance.UpdateSwapchainInfo(physicalDevice, surface);

		if constexpr (GRAPHICS_VALIDATION_LEVEL > 0)
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

	CHECKF(!graphicsDeviceCandidates.empty(), "Failed to find a suitable GPU!");

	return std::get<0>(graphicsDeviceCandidates.front());
}

template <>
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

template <>
void CreateQueues(Rhi<kVk>& rhi)
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
		QueueTimelineContext<kVk>{{}, {rhi.device, SemaphoreCreateDesc<kVk>{VK_SEMAPHORE_TYPE_TIMELINE}}});

	auto [computeQueuesIt, computeQueuesWasInserted] = queues.emplace(
		kQueueTypeCompute,
		QueueTimelineContext<kVk>{{}, {rhi.device, SemaphoreCreateDesc<kVk>{VK_SEMAPHORE_TYPE_TIMELINE}}});

	auto [transferQueuesIt, transferQueuesWasInserted] = queues.emplace(
		kQueueTypeTransfer,
		QueueTimelineContext<kVk>{{}, {rhi.device, SemaphoreCreateDesc<kVk>{VK_SEMAPHORE_TYPE_TIMELINE}}});

	auto IsDedicatedQueueFamily = [](const QueueFamilyDesc<kVk>& queueFamily, VkQueueFlagBits type)
	{
		return (queueFamily.flags >= type) && (queueFamily.queueCount > 0);
	};

	auto& [graphicsQueueInfos, graphicsSemaphore] = queues[kQueueTypeGraphics];
	auto& [computeQueueInfos, computeSemaphore] = queues[kQueueTypeCompute];
	auto& [transferQueueInfos, transferSemaphore] = queues[kQueueTypeTransfer];

	const auto& queueFamilies = rhi.device->GetQueueFamilies();
	for (unsigned queueFamilyIt = 0; queueFamilyIt < queueFamilies.size(); queueFamilyIt++)
	{
		const auto& queueFamily = queueFamilies[queueFamilyIt];

		auto queueCount = queueFamily.queueCount;

		if (IsDedicatedQueueFamily(queueFamily, VK_QUEUE_GRAPHICS_BIT))
		{
			for (unsigned queueIt = 0; queueIt < queueCount; queueIt++)
			{
				auto& [queue, syncInfo] = graphicsQueueInfos.emplace_back(QueueHostSyncContext<kVk>{});
				queue = Queue<kVk>(
					rhi.device,
					CommandPoolCreateDesc<kVk>{cmdPoolCreateFlags, queueFamilyIt, 2},
					QueueCreateDesc<kVk>{queueIt, queueFamilyIt});
			}
		}
		else if (IsDedicatedQueueFamily(queueFamily, VK_QUEUE_COMPUTE_BIT))
		{
			for (unsigned queueIt = 0; queueIt < queueCount; queueIt++)
			{
				auto& [queue, syncInfo] = computeQueueInfos.emplace_back(QueueHostSyncContext<kVk>{});
				queue = Queue<kVk>(
					rhi.device,
					CommandPoolCreateDesc<kVk>{cmdPoolCreateFlags, queueFamilyIt, 1},
					QueueCreateDesc<kVk>{queueIt, queueFamilyIt});
			}
		}
		else if (IsDedicatedQueueFamily(queueFamily, VK_QUEUE_TRANSFER_BIT))
		{
			for (unsigned queueIt = 0; queueIt < queueCount; queueIt++)
			{
				auto& [queue, syncInfo] = transferQueueInfos.emplace_back(QueueHostSyncContext<kVk>{});
				queue = Queue<kVk>(
					rhi.device,
					CommandPoolCreateDesc<kVk>{cmdPoolCreateFlags, queueFamilyIt, 1},
					QueueCreateDesc<kVk>{queueIt, queueFamilyIt});
			}
		}
	}

	CHECKF(!graphicsQueueInfos.empty(), "Failed to find a suitable graphics queue!");

	if (computeQueueInfos.empty())
	{
		CHECKF(graphicsQueueInfos.size() >= (minComputeQueueCount + minGraphicsQueueCount), "Failed to find a suitable compute queue!");

		computeQueueInfos.emplace_back(std::make_pair(
			std::move(graphicsQueueInfos.back().first),
			QueueHostSyncInfo<kVk>{}));
		graphicsQueueInfos.pop_back();
	}

	if (transferQueueInfos.empty())
	{
		CHECKF(graphicsQueueInfos.size() >= (minTransferQueueCount + minGraphicsQueueCount), "Failed to find a suitable transfer queue!");

		transferQueueInfos.emplace_back(std::make_pair(
			std::move(graphicsQueueInfos.back().first),
			QueueHostSyncInfo<kVk>{}));
		graphicsQueueInfos.pop_back();
	}
}

template <>
void CreateWindowDependentObjects(Rhi<kVk>& rhi)
{
	ZoneScopedN("rhiapplication::createWindowDependentObjects");

	auto colorImage = std::make_shared<Image<kVk>>(
		rhi.device,
		ImageCreateDesc<kVk>{
			{{rhi.windows.at(GetCurrentWindow()).GetConfig().swapchainConfig.extent}},
			rhi.windows.at(GetCurrentWindow()).GetConfig().swapchainConfig.surfaceFormat.format,
			VK_IMAGE_TILING_OPTIMAL,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
				VK_IMAGE_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
			VK_IMAGE_LAYOUT_UNDEFINED,
			"Main RT Color"});

	auto depthStencilImage = std::make_shared<Image<kVk>>(
		rhi.device,
		ImageCreateDesc<kVk>{
			{{rhi.windows.at(GetCurrentWindow()).GetConfig().swapchainConfig.extent}},
			FindSupportedFormat(
				rhi.device->GetPhysicalDevice(),
				{VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT},
				VK_IMAGE_TILING_OPTIMAL,
				VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
					VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT),
			VK_IMAGE_TILING_OPTIMAL,
			VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
				VK_IMAGE_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
			VK_IMAGE_LAYOUT_UNDEFINED,
			"Main RT DepthStencil"});

	rhi.renderImageSet =
		std::make_shared<RenderImageSet<kVk>>(rhi.device, std::vector{colorImage, depthStencilImage});

	rhi.pipeline->SetRenderTarget(rhi.renderImageSet);
}

template <>
Window<kVk> CreateRhiWindow(
	const std::shared_ptr<Device<kVk>>& device,
	SurfaceHandle<kVk>&& surface,
	typename Window<kVk>::ConfigFile&& windowConfig,
	WindowState&& windowState)
{
	return Window<kVk>(
		device,
		std::forward<SurfaceHandle<kVk>>(surface),
		std::forward<Window<kVk>::ConfigFile>(windowConfig),
		std::forward<WindowState>(windowState));
}

template <>
std::unique_ptr<Pipeline<kVk>> CreatePipeline(const std::shared_ptr<Device<kVk>>& device)
{
	return std::make_unique<Pipeline<kVk>>(
		device,
		PipelineConfiguration<kVk>{(std::get<std::filesystem::path>(Application::Instance().lock()->Env().variables["UserProfilePath"]) / "pipeline.cache").string()});
}

template <>
std::shared_ptr<Device<kVk>> CreateDevice(
	const std::shared_ptr<Instance<kVk>>& instance,
	uint32_t physicalDeviceIndex)
{
	return std::make_shared<Device<kVk>>(
		instance,
		DeviceConfiguration<kVk>{physicalDeviceIndex});
}

template <>
std::shared_ptr<Instance<kVk>> CreateInstance(std::string_view name)
{
	return std::make_shared<Instance<kVk>>(
		InstanceConfiguration<kVk>{
			name.data(),
			"speedo",
			ApplicationInfo<kVk>{
				VK_STRUCTURE_TYPE_APPLICATION_INFO,
				nullptr,
				nullptr,
				VK_MAKE_VERSION(1, 0, 0),
				nullptr,
				VK_MAKE_VERSION(1, 0, 0),
				VK_API_VERSION_1_2}});
				//VK_API_VERSION_1_3}});
}

} // namespace rhi
