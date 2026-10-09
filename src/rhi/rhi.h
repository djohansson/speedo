#pragma once

#include <rhi/capi.h>
#include <rhi/device.h>
#include <rhi/instance.h>
#include <rhi/rhibase.h>
#include <rhi/shaderset.h>
#include <rhi/types.h>
#include <rhi/swapchain.h>

#include <core/assert.h>

#include <string_view>
#include <utility>

namespace rhi
{

struct RHIInitializationData
{
	std::string_view name;
	// the window drawn to (the platform's, see platform::Window): its surface picks the devices that can present to it,
	// and gets a swapchain
	WindowHandle window{};
	SurfaceHandle<kVk> surface{}; // the window's, made by the constructor
	// the descriptors the primary device's pipeline allocates its descriptor sets from, per type
	std::vector<DescriptorPoolSize> descriptorPoolSizes;
};

// the graphics api the code above rhi uses (one per build)
inline constexpr GraphicsApi kGraphicsApi = kVk;
// the shader binaries it takes
inline constexpr ShaderFormat kShaderFormat = ShaderFormat::kSpirv16;

template <GraphicsApi G>
class RHI;

// the RHI of a graphics api while one exists, from the start of its construction to the end of its destruction (so
// that the objects it creates and destroys can find their instance and devices through it), else null
template <GraphicsApi G>
[[nodiscard]] RHI<G>* GetRHI() noexcept;

namespace detail
{

// registers an RHI for GetRHI for as long as it lives, as its first member
template <GraphicsApi G>
class RHIRegistration final
{
public:
	explicit RHIRegistration(RHI<G>* rhi) noexcept;
	RHIRegistration(const RHIRegistration&) = delete;
	RHIRegistration(RHIRegistration&&) = delete;
	~RHIRegistration();

	RHIRegistration& operator=(const RHIRegistration&) = delete;
	RHIRegistration& operator=(RHIRegistration&&) = delete;
};

} // namespace detail

template <GraphicsApi G>
class RHI final : public RHIBase 
{
public:
	explicit RHI(RHIInitializationData&& initData);
	RHI(const RHI&) = delete;
	RHI(RHI&& other) noexcept = delete;
	~RHI() = default;

	[[maybe_unused]] RHI& operator=(const RHI&) = delete;
	[[maybe_unused]] RHI& operator=(RHI&& other) noexcept = delete;

	[[nodiscard]] auto& GetInstance() noexcept { return myInstance; }
	[[nodiscard]] const auto& GetInstance() const noexcept { return myInstance; }
	
	// the swapchain drawn to window
	[[nodiscard]] Swapchain<G>& GetSwapchain(WindowHandle window) { return const_cast<Swapchain<G>&>(std::as_const(*this).GetSwapchain(window)); }
	[[nodiscard]] const Swapchain<G>& GetSwapchain(WindowHandle window) const
	{
		auto swapchainIt = std::ranges::find(mySwapchains, window, &WindowSwapchain::window);
		ASSERTF(swapchainIt != mySwapchains.end(), "Window {} has no swapchain", window);
		return swapchainIt->swapchain;
	}

	[[nodiscard]] auto& GetDevice(DeviceHandle<G> handle) noexcept { return const_cast<Device<G>&>(std::as_const(*this).GetDevice(handle)); }
	[[nodiscard]] const auto& GetDevice(DeviceHandle<G> handle) const noexcept
	{
		auto deviceIt = std::ranges::find_if(myDevices, [handle](const auto& device){ return device == handle; });
		ASSERTF(deviceIt != myDevices.end(), "Device {} is not registered in RHI (accessed during Device construction?)", static_cast<const void*>(handle));
		return *deviceIt;
	}

	[[nodiscard]] auto& GetPrimaryDevice() noexcept { return myDevices.front(); }
	[[nodiscard]] const auto& GetPrimaryDevice() const noexcept { return myDevices.front(); }

private:
	detail::RHIRegistration<G> myRegistration{this}; // first: constructed before, and destroyed after, the rest
	Instance<G> myInstance;
	std::vector<Device<G>> myDevices;
	struct WindowSwapchain
	{
		WindowHandle window{};
		Swapchain<G> swapchain;
	};
	std::vector<WindowSwapchain> mySwapchains;
};

} // namespace rhi
