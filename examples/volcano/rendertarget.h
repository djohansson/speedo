#pragma once

#include "device.h"
#include "gfx-types.h"

#include <memory>
#include <vector>

template <GraphicsBackend B>
struct RenderTargetCreateDesc
{
    std::shared_ptr<DeviceContext<B>> deviceContext;
    RenderPassHandle<B> renderPass = 0;
    Extent2d<B> imageExtent = {};
	Format<B> colorImageFormat;
    uint64_t colorImageCount = 0;
    const ImageHandle<B>* colorImages = nullptr;
    Format<B> depthImageFormat;
    ImageHandle<B> depthImage = 0; // optional
};

template <GraphicsBackend B>
class RenderTarget
{
    using DescType = RenderTargetCreateDesc<B>;

public:

	RenderTarget(RenderTarget<B>&& other) = default;
	RenderTarget(DescType&& desc);
	virtual ~RenderTarget();

    template <typename T = DescType>
    const T& getDesc() const { return static_cast<const T&>(myDesc); }
    const auto& getColorViews() const { return myColorViews; }
    const auto& getDepthView() const { return myDepthView; }
    const auto& getFrameBuffer() const { return myFrameBuffer; }

protected:
    
    RenderTargetCreateDesc<B> myDesc = {};

private:

	std::vector<ImageViewHandle<B>> myColorViews;
	ImageViewHandle<B> myDepthView = 0;
    FramebufferHandle<B> myFrameBuffer = 0;
};
