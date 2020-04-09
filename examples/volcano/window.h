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
struct RenderTargetBase
{
	uint32_t framebufferWidth = 0; // todo: rename
	uint32_t framebufferHeight = 0; // todo: rename
	
	std::vector<ImageHandle<B>> colorImages;
	std::vector<ImageViewHandle<B>> colorViews;
	
	ImageHandle<B> depthImage = 0;
	ImageViewHandle<B> depthView = 0;
};

template <GraphicsBackend B>
struct Window : RenderTargetBase<B>
{
	uint32_t width = 0; // todo: rename
	uint32_t height = 0; // todo: rename

	SurfaceHandle<B> surface = 0;
	
	std::unique_ptr<SwapchainContext<B>> swapchain;
	std::unique_ptr<Texture<B>> zBuffer;
	
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
};
