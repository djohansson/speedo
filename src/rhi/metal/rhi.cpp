// the Metal backend's RHI (docs/backends-plan.md): the instance, the device and its queues and pipeline, and with a
// window its swapchain, of the window's CAMetalLayer (PlatformCreateMetalLayer)
#include <rhi/metal/backend.h>

#include <rhi/rhi.h>
#include <rhi/swapchain.h>

#include <core/assert.h>
#include <core/uuids_extra.h>

#include <platform/capi.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <format>
#include <print>
#include <string_view>
#include <vector>

namespace rhi
{

namespace detail
{

static std::atomic<RHI<kMetal>*> gMetalRHI = nullptr;

template <>
RHIRegistration<kMetal>::RHIRegistration(RHI<kMetal>* rhi) noexcept
{
	[[maybe_unused]] RHI<kMetal>* expected = nullptr;
	ENSUREF(gMetalRHI.compare_exchange_strong(expected, rhi), "only one RHI<kMetal> may exist at a time");
}

template <>
RHIRegistration<kMetal>::~RHIRegistration()
{
	gMetalRHI = nullptr;
}

// the device everything draws on: the instance's first (the system's default device, see Instance)
static std::vector<Device<kMetal>> DetectAndCreateDevices(Instance<kMetal>& instance)
{
	const auto& physicalDevices = instance.GetPhysicalDevices();
	ENSUREF(!physicalDevices.empty(), "Failed to find a Metal device");

	std::vector<Device<kMetal>> devices;
	devices.emplace_back(
		DeviceCreateDesc<kMetal>{
			ObjectCreateDesc<kMetal>{
				.uuid = uuids::NewUuid(),
				.instance = instance,
				.name = "Device 0",
			},
			physicalDevices.front(),
		},
		instance);
	return devices;
}

// the swapchain of the window's layer: Vulkan's backend's choices (its images B8G8R8A8 unorm, which the frame's final
// pass writes encoded; fifo; 3 images), with its overrides, SPEEDO_PRESENT_MODE=fifo or immediate (Metal has no mailbox
// or fifo relaxed) and SPEEDO_SWAPCHAIN_IMAGE_COUNT=<count> (2 or 3: the drawables a layer keeps)
static SwapchainCreateDesc<kMetal> DetectSuitableSwapchain(Device<kMetal>& device, CA::MetalLayer* layer, uint32_t maxImageCount)
{
	SwapchainCreateDesc<kMetal> config{
		RenderTargetCreateDesc<kMetal>{device.CreateDeviceObjectCreateDesc("Swapchain"), {}, {Format::kUndefined}, {ImageLayout::kUndefined}, {ImageAspect::kColor}},
		layer,
		{.format = Format::kB8G8R8A8Unorm},
		metal::PresentMode::kFifo,
		maxImageCount};
	auto imageCountLimit = maxImageCount != 0 ? std::clamp(maxImageCount, 2U, 3U) : 3U;
	config.images.resize(std::min(3U, imageCountLimit));
	if (const char* presentMode = std::getenv("SPEEDO_PRESENT_MODE"); presentMode != nullptr)
	{
		std::string_view name(presentMode);
		if (name == "immediate")
			config.presentMode = metal::PresentMode::kImmediate;
		else if (name != "fifo")
			std::println(stderr, "SPEEDO_PRESENT_MODE={}: Metal presents fifo or immediate", presentMode);
	}
	if (const char* imageCount = std::getenv("SPEEDO_SWAPCHAIN_IMAGE_COUNT"); imageCount != nullptr)
		config.images.resize(std::clamp(static_cast<uint32_t>(std::strtoul(imageCount, nullptr, 10)), 2U, imageCountLimit));
	return config;
}

} // namespace detail

template <>
RHI<kMetal>* GetRHI<kMetal>() noexcept
{
	return detail::gMetalRHI;
}

template <>
RHI<kMetal>::RHI(RHIInitializationData&& initData, SurfaceHandle<kMetal> surface)
	: myInstance(InstanceCreateDesc<kMetal>{std::string(initData.name), "speedo", initData.window != 0})
	, myDevices{detail::DetectAndCreateDevices(myInstance)}
{
	// device objects find their Device through GetRHI<kMetal>(), so these come once the devices are registered
	for (auto& device : myDevices)
	{
		device.InternalCreateQueues();
		device.InternalCreatePipeline(std::vector(initData.descriptorPoolSizes));
	}

	if (initData.window != 0)
		mySwapchains.push_back({
			.window = initData.window,
			.swapchain = Swapchain<kMetal>(detail::DetectSuitableSwapchain(GetPrimaryDevice(), surface, initData.maxSwapchainImageCount))});
}

template <>
RHI<kMetal>::RHI(RHIInitializationData&& initData)
	: RHI(
		std::forward<RHIInitializationData>(initData),
		// the window's layer (none when headless), retained for the swapchain, which releases it
		initData.window != 0 ? static_cast<CA::MetalLayer*>(PlatformCreateMetalLayer(initData.window)) : nullptr)
{}

} // namespace rhi
