#include <rhi/rhi.h>
#include <rhi/capi.h>
#include <rhi/swapchain.h>
#include <rhi/vulkan/utils.h>

#include <core/assert.h>

#include <cstdint>
#include <format>
#include <iostream>
#include <tuple>
#include <vector>

namespace rhi
{

namespace detail
{

std::vector<Device<kVk>> DetectAndCreateDevices(Instance<kVk>& instance, SurfaceHandle<kVk> surface)
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

	std::vector<Device<kVk>> devices;
	devices.reserve(graphicsDeviceCandidates.size());
	for (const auto& [physicalDeviceIt, queueFamilyIt] : graphicsDeviceCandidates)
		devices.emplace_back(
			DeviceCreateDesc<kVk>{
				ObjectCreateDesc<kVk>{
					.uuid = uuids::NewUuid(),
					.instance = instance,
					.name = std::format("Device {}", physicalDeviceIt)},
				physicalDevices[physicalDeviceIt],
			},
			instance);

	return devices;
}

SwapchainCreateDesc<kVk> DetectSuitableSwapchain(
	Instance<kVk>& instance,
	Device<kVk>& device,
	SurfaceHandle<kVk> surface)
{
	const auto& swapchainInfo =
		instance.GetSwapchainInfo(device.GetPhysicalDevice(), surface);

	SwapchainCreateDesc<kVk> config{
		RenderTargetCreateDesc<kVk>
		{
			device.CreateDeviceObjectCreateDesc("Swapchain"),
			swapchainInfo.capabilities.currentExtent,
		 	{VK_FORMAT_UNDEFINED},
			{VK_IMAGE_LAYOUT_UNDEFINED},
			{VK_IMAGE_ASPECT_COLOR_BIT}
		},
		surface,
		{.format=VK_FORMAT_UNDEFINED, .colorSpace=VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
		VK_PRESENT_MODE_FIFO_KHR
	};

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
	config.images.resize(swapchainInfo.capabilities.minImageCount);
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
				config.images.resize(3);
			}
			else
			{
				config.images.resize(2);
			}
			break;
		}
	}

	return config;
};

} // namespace detail

template <>
RHI<kVk>::RHI(RHIInitializationData&& initData)
	: myInstance(InstanceCreateDesc<kVk>{std::string(initData.name), "speedo"})
	, myDevices{[&initData](Instance<kVk>& instance)
	{
		using namespace detail;

		initData.windowHandle = initData.createWindowFunc(&initData.windowState);
		initData.surface = CreateSurface(instance, &instance.GetHostAllocationCallbacks(), initData.windowHandle);

		return DetectAndCreateDevices(instance, initData.surface);
	}(myInstance)}
{
	using namespace detail;

	// device objects resolve their Device via RHIApplication::GetRHI<kVk>().GetDevice(), so these
	// can only be created once the devices are registered in myDevices.
	for (auto& device : myDevices)
	{
		device.InternalCreateQueues();
		device.InternalCreatePipeline();
	}

	myWindows.emplace_back(
		WindowCreateDesc<kVk>{
			GetPrimaryDevice().CreateDeviceObjectCreateDesc("Window"),
			initData.windowHandle,
			{initData.windowState.xscale, initData.windowState.yscale},
			{.width = 1, .height = 1},
			initData.windowState.fullscreenEnabled > 0,
		},
		DetectSuitableSwapchain(myInstance, GetPrimaryDevice(), initData.surface),
		WindowState{initData.windowState}
	);

	SetCurrentWindow(initData.windowHandle);
	//(std::get<std::filesystem::path>(Application::Get()->GetEnv().variables["UserProfilePath"]) / "pipeline.cache").string()
}

} // namespace rhi
