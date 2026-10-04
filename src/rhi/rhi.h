#pragma once

#include <rhi/capi.h>
#include <rhi/device.h>
#include <rhi/instance.h>
#include <rhi/rhibase.h>
#include <rhi/types.h>
#include <rhi/window.h>

#include <core/assert.h>

#include <string_view>
#include <utility>

namespace rhi
{

inline constexpr int kDefaultWindowWidth = 1280;
inline constexpr int kDefaultWindowHeight = 720;

struct RHIInitializationData
{
	std::string_view name;
	CreateWindowFunc createWindowFunc{};
	WindowState windowState{.x = 0, .y = 0, .width = kDefaultWindowWidth, .height = kDefaultWindowHeight, .fullscreenEnabled = 0U};
	WindowHandle windowHandle{};
	SurfaceHandle<kVk> surface{};
};

// the graphics api the code above rhi uses (one per build)
inline constexpr GraphicsApi kGraphicsApi = kVk;

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
	
	[[nodiscard]] auto& GetWindows() noexcept { return myWindows; }
	[[nodiscard]] const auto& GetWindows() const noexcept { return myWindows; }
	[[nodiscard]] Window<G>& GetWindow(WindowHandle handle) { return *std::ranges::find_if(myWindows, [handle](const auto& window){ return window == handle; }); }
	[[nodiscard]] const Window<G>& GetWindow(WindowHandle handle) const { return *std::ranges::find_if(myWindows, [handle](const auto& window){ return window == handle; }); }

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
	std::vector<Window<G>> myWindows;
	CreateWindowFunc myCreateWindowFunc;
};

} // namespace rhi
