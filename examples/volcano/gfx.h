#pragma once

#include "gfx-types.h"

#include <tuple>

template <GraphicsBackend B>
std::tuple<SwapchainInfo<B>, int, PhysicalDeviceProperties<B>> getSuitableSwapchainAndQueueFamilyIndex(
	SurfaceHandle<B> surface, PhysicalDeviceHandle<B> device);
