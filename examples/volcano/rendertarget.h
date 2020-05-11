#pragma once

#include "device.h"
#include "gfx-types.h"

#include <memory>
#include <optional>
#include <vector>

template <GraphicsBackend B>
struct RenderTargetCreateDesc
{
    RenderPassHandle<B> renderPass = 0; // optional
    Extent2d<B> imageExtent = {};
	Format<B> colorImageFormat = {};
    std::vector<ImageHandle<B>> colorImages;
    Format<B> depthImageFormat = {};
    ImageHandle<B> depthImage = 0; // optional
};

template <GraphicsBackend B>
class RenderTarget
{
public:

    virtual ~RenderTarget();

    const auto& getImageExtent() const { return myDesc.imageExtent; }
    const auto& getRenderPass() const { return myDesc.renderPass; }
    const auto& getFrameBuffer() const { return myFrameBuffer; }
    const auto& getColorViews() const { return myColorViews; }
    const auto& getDepthView() const { return myDepthView; }

protected:

    RenderTarget(
        RenderTarget<B>&& other,
        const RenderTargetCreateDesc<B>* myDescPtr);
    RenderTarget(
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        const RenderTargetCreateDesc<B>& desc,
        const RenderTargetCreateDesc<B>* myDescPtr);

    const auto& getDeviceContext() const { return myDeviceContext; }

private:

    std::shared_ptr<DeviceContext<B>> myDeviceContext;
    const RenderTargetCreateDesc<B>& myDesc; // do NOT use myDesc in constructor or destructor - object pointed to may not be valid during construction/destruction
    FramebufferHandle<B> myFrameBuffer = 0;
	std::vector<ImageViewHandle<B>> myColorViews;
	std::optional<ImageViewHandle<B>> myDepthView;
};

template <typename CreateDescType, GraphicsBackend B>
class RenderTargetImpl : public RenderTarget<B>
{ };

#include "rendertarget-vulkan.inl"
