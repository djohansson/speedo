#pragma once

#include "buffer.h"
#include "frame.h"
#include "gfx-types.h"
#include "swapchain.h"
#include "texture.h"
#include "view.h"

#include <optional>
#include <memory>
#include <vector>

template <GraphicsBackend B>
struct Window
{
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t framebufferWidth = 0;
	uint32_t framebufferHeight = 0;

	SurfaceHandle<B> surface = 0;
	SurfaceFormat<B> surfaceFormat = {};
	PresentMode<B> presentMode = {};
	uint32_t frameCount = 0;

	SwapchainContext<B> swapchain = {};

	std::shared_ptr<Texture<B>> zBuffer;
	ImageViewHandle<B> zBufferView = 0;
	
	std::vector<View> views;
	std::optional<size_t> activeView;

	// buffer for all views.
	std::shared_ptr<Buffer<B>> viewBuffer;
	//

	bool clearEnable = true;
    ClearValue<B> clearValue = {};
    
	uint32_t frameIndex = 0; // Current frame being rendered to (0 <= frameIndex < frameCount)
	uint32_t lastFrameIndex = 0; // Last frame being rendered to (0 <= frameIndex < frameCount)
	std::vector<Frame<B>> frames; // frames.count == frameCount

	bool imguiEnable = true;
	bool createStateFlag = false;
};
