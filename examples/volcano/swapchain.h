#pragma once

#include "device.h"
#include "frame.h"
#include "types.h"

#include <memory>

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
	const auto& getFrames() const { return myFrames; }

	std::tuple<bool, uint32_t, uint64_t> flip() const;
	uint64_t submit(
		const std::shared_ptr<CommandContext<B>>& commandContext,
		uint32_t frameIndex,
		uint64_t waitTimelineValue);
	void present(uint32_t frameIndex);

private:

	const SwapchainCreateDesc<B> myDesc = {};
	SwapchainHandle<B> mySwapchain = 0;
	std::vector<std::unique_ptr<Frame<B>>> myFrames;
	uint32_t myLastFrameIndex = 0;
};
