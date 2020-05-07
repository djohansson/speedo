#pragma once

#include "device.h"
#include "gfx-types.h"

#include <memory>
#include <vector>

template <GraphicsBackend B>
struct RenderTargetCreateDesc
{
    std::shared_ptr<DeviceContext<B>> deviceContext;
    RenderPassHandle<B> renderPass = 0; // optional
    Extent2d<B> imageExtent = {};
	Format<B> colorImageFormat = {};
    std::vector<ImageHandle<B>> colorImages;
    Format<B> depthImageFormat = {};
    ImageHandle<B> depthImage = 0; // optional
};

template <GraphicsBackend B>
struct RenderTarget
{
    Extent2d<B> imageExtent = {};
    RenderPassHandle<B> renderPass = 0;
    FramebufferHandle<B> frameBuffer = 0;
	std::vector<ImageViewHandle<B>> colorViews;
	std::optional<ImageViewHandle<B>> depthView;
};

template <typename CreateDescType, GraphicsBackend B>
class RenderTargetImpl : public RenderTarget<B>
{ };

#include "rendertarget-vulkan.h"
