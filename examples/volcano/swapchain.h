#pragma once

#include "device.h"
#include "gfx-types.h"

#include <memory>
#include <vector>

template <GraphicsBackend B>
struct SwapchainCreateDesc
{
	Extent2d<B> imageExtent = {};
	SwapchainHandle<B> previous = 0;
};

template <GraphicsBackend B>
class SwapchainContext
{
public:

	SwapchainContext(SwapchainContext&& other) = default;
	SwapchainContext(
		const std::shared_ptr<DeviceContext<B>>& deviceContext,
		SwapchainCreateDesc<B>&& desc);
    ~SwapchainContext();

    const auto& getDesc() const { return myDesc; }
	const auto& getSwapchain() const { return mySwapchain; }

	auto detatch()
	{
		auto handle = mySwapchain;
		mySwapchain = 0;
		return handle;
	}

private:

	std::shared_ptr<DeviceContext<B>> myDeviceContext;
	const SwapchainCreateDesc<B> myDesc = {};
	SwapchainHandle<B> mySwapchain = 0;
};
