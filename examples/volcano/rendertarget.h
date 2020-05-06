#pragma once

#include "device.h"
#include "gfx-types.h"

#include <memory>
#include <vector>

template <GraphicsBackend B>
struct RenderTargetCreateDesc
{
    std::shared_ptr<DeviceContext<B>> deviceContext;
    Extent2d<B> imageExtent = {};
	Format<B> colorImageFormat;
    uint64_t colorImageCount = 0;
    const ImageHandle<B>* colorImages = nullptr;
    Format<B> depthImageFormat;
    ImageHandle<B> depthImage = 0; // optional
    RenderPassHandle<B> renderPass = 0; // optional
};

template <GraphicsBackend B>
struct RenderTargetBase
{
    const auto& getImageExtent() const { return myImageExtent; }
    const auto& getRenderPass() const { return myRenderPass; }
    const auto& getFrameBuffer() const { return myFrameBuffer; }
    const auto& getColorViews() const { return myColorViews; }
    const auto& getDepthView() const { return myDepthView; }

    Extent2d<B> myImageExtent = {};
    RenderPassHandle<B> myRenderPass = 0;
    FramebufferHandle<B> myFrameBuffer = 0;
	std::vector<ImageViewHandle<B>> myColorViews;
	std::optional<ImageViewHandle<B>> myDepthView;
};

template <typename T, GraphicsBackend B>
class RenderTarget : public RenderTargetBase<B>
{ };

#include "rendertarget-vulkan.h"
