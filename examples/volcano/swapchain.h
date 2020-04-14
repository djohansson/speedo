#pragma once

#include "device.h"
#include "gfx-types.h"
#include "utils.h"

#include <memory>
#include <vector>

template <GraphicsBackend B>
struct SwapchainDesc
{
	std::shared_ptr<DeviceContext<B>> deviceContext;
	SwapchainHandle<B> previous = 0;
	Extent2d<B> imageExtent = {};
};

template <GraphicsBackend B>
class SwapchainContext : Noncopyable
{
public:

	SwapchainContext(SwapchainDesc<B>&& desc);
    ~SwapchainContext();

    const auto& getSwapchainDesc() const { return mySwapchainDesc; }
	const auto& getSwapchain() const { return mySwapchain; }

	auto detatch()
	{
		auto handle = mySwapchain;
		mySwapchain = 0;
		return handle;
	}

private:

	const SwapchainDesc<B> mySwapchainDesc = {};
	SwapchainHandle<B> mySwapchain = 0;
};
