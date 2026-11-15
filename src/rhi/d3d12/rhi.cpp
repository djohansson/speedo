// the D3D12 backend's RHI (docs/backends-plan.md): the instance, the device and its queues and pipeline, and for a
// window its swapchain, on the window's HWND (PlatformGetWin32Window)
#include <rhi/d3d12/backend.h>

#include <rhi/rhi.h>
#include <rhi/swapchain.h>

#include <core/assert.h>
#include <core/uuids_extra.h>

#include <platform/capi.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <print>
#include <string_view>
#include <utility>
#include <vector>

namespace rhi
{

namespace detail
{

static std::atomic<RHI<kD3D12>*> gD3D12RHI = nullptr;

template <>
RHIRegistration<kD3D12>::RHIRegistration(RHI<kD3D12>* rhi) noexcept
{
	[[maybe_unused]] RHI<kD3D12>* expected = nullptr;
	ENSUREF(gD3D12RHI.compare_exchange_strong(expected, rhi), "only one RHI<kD3D12> may exist at a time");
}

template <>
RHIRegistration<kD3D12>::~RHIRegistration()
{
	gD3D12RHI = nullptr;
}

// the device everything draws on: the instance's first adapter (the highest performance one, see Instance)
static std::vector<Device<kD3D12>> DetectAndCreateDevices(Instance<kD3D12>& instance)
{
	const auto& physicalDevices = instance.GetPhysicalDevices();
	ENSUREF(!physicalDevices.empty(), "Failed to find a D3D12 adapter (feature level 12.1; WARP with SPEEDO_D3D12_WARP=1 or a d3d10warp.dll's path)");

	std::vector<Device<kD3D12>> devices;
	devices.emplace_back(
		DeviceCreateDesc<kD3D12>{
			ObjectCreateDesc<kD3D12>{
				.uuid = uuids::NewUuid(),
				.instance = instance,
				.name = "Device 0",
			},
			physicalDevices.front(),
		},
		instance);
	return devices;
}

// the swapchain of the window: Vulkan's backend's choices (its images B8G8R8A8 unorm, which the frame's final pass writes
// encoded; fifo; 3 images), with its overrides, SPEEDO_PRESENT_MODE=fifo, mailbox or immediate (where the system allows
// tearing) and SPEEDO_SWAPCHAIN_IMAGE_COUNT=<count> (2 to 16)
static SwapchainCreateDesc<kD3D12> DetectSuitableSwapchain(Device<kD3D12>& device, HWND window, uint32_t maxImageCount)
{
	SwapchainCreateDesc<kD3D12> config{
		RenderTargetCreateDesc<kD3D12>{device.CreateDeviceObjectCreateDesc("Swapchain"), {}, {Format::kUndefined}, {ImageLayout::kUndefined}, {ImageAspect::kColor}},
		window,
		{.format = Format::kB8G8R8A8Unorm},
		d3d12::PresentMode::kFifo,
		maxImageCount};
	constexpr uint32_t kMaxImageCount = 16;
	auto imageCountLimit = maxImageCount != 0 ? std::clamp(maxImageCount, 2U, kMaxImageCount) : kMaxImageCount;
	config.images.resize(std::min(3U, imageCountLimit));
	if (const char* presentMode = std::getenv("SPEEDO_PRESENT_MODE"); presentMode != nullptr)
	{
		std::string_view name(presentMode);
		if (name == "mailbox")
			config.presentMode = d3d12::PresentMode::kMailbox;
		else if (name == "immediate")
			config.presentMode = d3d12::PresentMode::kImmediate;
		else if (name != "fifo")
			std::println(stderr, "SPEEDO_PRESENT_MODE={}: D3D12 presents fifo, mailbox or immediate", presentMode);
	}
	if (const char* imageCount = std::getenv("SPEEDO_SWAPCHAIN_IMAGE_COUNT"); imageCount != nullptr)
		config.images.resize(std::clamp(static_cast<uint32_t>(std::strtoul(imageCount, nullptr, 10)), 2U, imageCountLimit));
	return config;
}

} // namespace detail

template <>
RHI<kD3D12>* GetRHI<kD3D12>() noexcept
{
	return detail::gD3D12RHI;
}

template <>
RHI<kD3D12>::RHI(RHIInitializationData&& initData, SurfaceHandle<kD3D12> surface)
	: myInstance(InstanceCreateDesc<kD3D12>{std::string(initData.name), "speedo", initData.window != 0})
	, myDevices{detail::DetectAndCreateDevices(myInstance)}
{
	// device objects find their Device through GetRHI<kD3D12>(), so these come once the devices are registered
	for (auto& device : myDevices)
	{
		device.InternalCreateQueues();
		device.InternalCreatePipeline(std::vector(initData.descriptorPoolSizes));
	}

	if (initData.window != 0)
		mySwapchains.push_back({
			.window = initData.window,
			.swapchain = Swapchain<kD3D12>(detail::DetectSuitableSwapchain(GetPrimaryDevice(), surface, initData.maxSwapchainImageCount))});
}

template <>
RHI<kD3D12>::RHI(RHIInitializationData&& initData)
	: RHI(
		std::forward<RHIInitializationData>(initData),
		// the window's HWND (none when headless)
		initData.window != 0 ? static_cast<HWND>(PlatformGetWin32Window(initData.window)) : nullptr)
{}

} // namespace rhi
