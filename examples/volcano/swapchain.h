#pragma once

#include "gfx-types.h"
#include "utils.h"

#include <vector>

template <GraphicsBackend B>
struct SwapchainConfiguration
{
	SurfaceCapabilities<B> capabilities = {};
	std::vector<SurfaceFormat<B>> formats;
	std::vector<PresentMode<B>> presentModes;
	uint8_t selectedFormatIndex = 0;
	uint8_t selectedPresentModeIndex = 0;
    uint8_t selectedImageCount = 2;
	
	inline SurfaceFormat<B> selectedFormat() const { return formats[selectedFormatIndex]; };
	inline PresentMode<B> selectedPresentMode() const { return presentModes[selectedPresentModeIndex]; };
};

template <GraphicsBackend B>
struct SwapchainCreateDesc
{
	SwapchainConfiguration<B> configuration = {};
	DeviceHandle<B> device = 0;
	AllocatorHandle<B> allocator = 0;
	SurfaceHandle<B> surface = 0;
	SwapchainHandle<B> previous = 0;
	Extent2d<B> imageExtent = {};
};

template <GraphicsBackend B>
class SwapchainContext : Noncopyable
{
public:

	SwapchainContext(SwapchainCreateDesc<B>&& desc);
    ~SwapchainContext();

    const auto& getDesc() const { return myDesc; }
	const auto& getSwapchain() const { return mySwapchain; }
	const std::vector<ImageHandle<B>> getColorImages() const;

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
