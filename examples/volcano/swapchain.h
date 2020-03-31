#pragma once

#include "gfx-types.h"

#include <vector>

template <GraphicsBackend B>
struct SwapchainInfo
{
	SurfaceCapabilities<B> capabilities = {};
	std::vector<SurfaceFormat<B>> formats;
	std::vector<PresentMode<B>> presentModes;
};

template <GraphicsBackend B>
struct SwapchainContext
{
	SwapchainInfo<B> info = {};
	SwapchainHandle<B> swapchain = 0;

	std::vector<ImageHandle<B>> colorImages;
	std::vector<ImageViewHandle<B>> colorImageViews;
};
