#pragma once

#include "device.h"
#include "gfx-types.h"
#include "utils.h"

#include <memory>
#include <vector>

template <GraphicsBackend B>
struct RenderTargetDesc
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
class RenderTarget : Noncopyable
{
public:

	RenderTarget(RenderTarget<B>&& other)
    : myDesc(other.myDesc)
    , myColorViews(std::move(other.myColorViews))
    , myDepthView(other.myDepthView)
    , myFrameBuffer(other.myFrameBuffer)
    {
		other.myDesc = {};
        other.myDepthView = 0;
        other.myFrameBuffer = 0;
	}
	RenderTarget(RenderTargetDesc<B>&& desc);
	virtual ~RenderTarget();

    const auto& getRenderTargetDesc() const { return myDesc; }
    const auto& getColorViews() const { return myColorViews; }
    const auto& getDepthView() const { return myDepthView; }
    const auto& getFrameBuffer() const { return myFrameBuffer; }

protected:
    
    RenderTargetDesc<B> myDesc = {};

private:

	std::vector<ImageViewHandle<B>> myColorViews;
	ImageViewHandle<B> myDepthView = 0;
    FramebufferHandle<B> myFrameBuffer = 0;
};
