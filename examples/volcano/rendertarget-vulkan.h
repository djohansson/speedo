#pragma once

#include "gfx.h"
#include "vk-utils.h"

#include <optional>
#include <utility>
#include <vector>

#include <Tracy.hpp>

template <typename T>
class RenderTarget<T, GraphicsBackend::Vulkan> : public RenderTargetBase<GraphicsBackend::Vulkan>
{
public:

    using CreateDescType = T;

	RenderTarget(RenderTarget<CreateDescType, GraphicsBackend::Vulkan>&& other);
	RenderTarget(CreateDescType&& desc);
	virtual ~RenderTarget();

    const auto& getDesc() const { return myDesc; }

protected:

    CreateDescType myDesc = {};
};

template <typename T>
RenderTarget<T, GraphicsBackend::Vulkan>::RenderTarget(RenderTarget<T, GraphicsBackend::Vulkan>&& other)
: RenderTargetBase(std::move(other))
, myDesc(std::move(other.myDesc))
{
    other.myDesc = {};
}

template <typename T>
RenderTarget<T, GraphicsBackend::Vulkan>::RenderTarget(CreateDescType&& desc)
: RenderTargetBase{desc.imageExtent, desc.renderPass}
, myDesc(std::move(desc))
{
    ZoneScopedN("RenderTarget()");

    std::vector<ImageViewHandle<GraphicsBackend::Vulkan>> attachments;

    myRenderPass = myDesc.renderPass;
    
    for (uint32_t i = 0; i < myDesc.colorImageCount; i++)
        myColorViews.emplace_back(createImageView2D(
            myDesc.deviceContext->getDevice(),
            myDesc.colorImages[i],
            myDesc.colorImageFormat,
            VK_IMAGE_ASPECT_COLOR_BIT));

    attachments.insert(std::end(attachments), std::begin(myColorViews), std::end(myColorViews));

    if (myDesc.depthImage)
    {
        VkImageAspectFlags depthAspectFlags = VK_IMAGE_ASPECT_DEPTH_BIT;
        if (hasStencilComponent(myDesc.depthImageFormat))
            depthAspectFlags |= VK_IMAGE_ASPECT_STENCIL_BIT;

        myDepthView = std::make_optional(createImageView2D(
            myDesc.deviceContext->getDevice(),
            myDesc.depthImage,
            myDesc.depthImageFormat,
            depthAspectFlags));

        attachments.push_back(myDepthView.value());
    }

    myFrameBuffer = createFramebuffer(
        myDesc.deviceContext->getDevice(),
        myDesc.renderPass,
        attachments.size(),
        attachments.data(),
        myDesc.imageExtent.width,
        myDesc.imageExtent.height,
        1);
}

template <typename T>
RenderTarget<T, GraphicsBackend::Vulkan>::~RenderTarget()
{
    ZoneScopedN("~RenderTarget()");

    for (const auto& colorView : myColorViews)
        vkDestroyImageView(myDesc.deviceContext->getDevice(), colorView, nullptr);

    if (myDepthView)
        vkDestroyImageView(myDesc.deviceContext->getDevice(), myDepthView.value(), nullptr);
    
    vkDestroyFramebuffer(myDesc.deviceContext->getDevice(), myFrameBuffer, nullptr);
}

extern template RenderTarget<RenderTargetCreateDesc<GraphicsBackend::Vulkan>, GraphicsBackend::Vulkan>::RenderTarget(
    RenderTarget<RenderTargetCreateDesc<GraphicsBackend::Vulkan>, GraphicsBackend::Vulkan>&& other);

extern template RenderTarget<RenderTargetCreateDesc<GraphicsBackend::Vulkan>, GraphicsBackend::Vulkan>::RenderTarget(CreateDescType&& desc);

extern template RenderTarget<RenderTargetCreateDesc<GraphicsBackend::Vulkan>, GraphicsBackend::Vulkan>::~RenderTarget();
