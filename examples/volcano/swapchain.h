#pragma once

#include "device.h"
#include "types.h"

#include <memory>
#include <vector>

template <GraphicsBackend B>
struct SwapchainCreateDesc : DeviceResourceCreateDesc<B>
{
	Extent2d<B> imageExtent = {};
	SwapchainHandle<B> previous = 0;
};

template <GraphicsBackend B>
class SwapchainContext : public DeviceResource<B>
{
public:

	SwapchainContext(SwapchainContext&& other) = default;
	SwapchainContext(
		const std::shared_ptr<DeviceContext<B>>& deviceContext,
		SwapchainCreateDesc<B>&& desc);
    ~SwapchainContext();

	SwapchainContext& operator=(SwapchainContext&& other) = default;

    const auto& getDesc() const { return myDesc; }
	const auto& getSwapchain() const { return mySwapchain; }

	auto detatch()
	{
		auto handle = mySwapchain;
		mySwapchain = 0;
		return handle;
	}

private:

	const SwapchainCreateDesc<B> myDesc = {};
	SwapchainHandle<B> mySwapchain = 0;
};
