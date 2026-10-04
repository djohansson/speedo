#include <rhi/window.h>
#include <rhi/rhi.h>

#include <rhi/vulkan/utils.h>


#include <string_view>

namespace rhi
{

IMPLEMENT_OBJECT_GETINSTANCE(Window<kVk>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(Window<kVk>);

template <>
void Window<kVk>::OnResizeFramebuffer(int width, int height)
{
	ASSERT(width > 0);
	ASSERT(height > 0);
	ASSERT(GetDesc().contentScale.x == myState.xscale);
	ASSERT(GetDesc().contentScale.y == myState.yscale);

	mySwapchain.CreateSwapchain();

	myState.width = static_cast<uint32_t>(static_cast<float>(mySwapchain.GetDesc().extent.width) / myState.xscale);
	myState.height = static_cast<uint32_t>(static_cast<float>(mySwapchain.GetDesc().extent.height) / myState.yscale);
}

template <>
Window<kVk>::Window(
	WindowCreateDesc<kVk>&& desc,
	SwapchainCreateDesc<kVk>&& swapchainDesc,
	WindowState&& state)
	: DeviceObject(std::forward<WindowCreateDesc<kVk>>(desc))
	, myState(std::forward<WindowState>(state))
	, mySwapchain(std::forward<SwapchainCreateDesc<kVk>>(swapchainDesc))
{
	ZoneScopedN("Window()");
}

template <>
Window<kVk>::Window(Window&& other) noexcept
	: DeviceObject(std::move(other))
	, myState(std::exchange(other.myState, {}))
	, mySwapchain(std::exchange(other.mySwapchain, {}))
	, myMinimized(std::exchange(other.myMinimized, {}))
{}

template <>
Window<kVk>::~Window()
{
	ZoneScopedN("~Window()");
}

template <>
void Window<kVk>::Swap(Window& other) noexcept
{
	DeviceObject::Swap(other);
	std::swap(myState, other.myState);
	std::swap(mySwapchain, other.mySwapchain);
	std::swap(myMinimized, other.myMinimized);
}

template <>
Window<kVk>& Window<kVk>::operator=(Window&& other) noexcept
{
	Swap(other);
	return *this;
}

} // namespace rhi
