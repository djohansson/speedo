#pragma once

#include "vk-utils.h"

#include <optional>
#include <utility>
#include <vector>


template <typename CreateDescType>
class RenderTargetImpl<CreateDescType, GraphicsBackend::Vulkan> : public RenderTarget<GraphicsBackend::Vulkan>
{
    using BaseType = RenderTarget<GraphicsBackend::Vulkan>;

public:

	RenderTargetImpl(RenderTargetImpl<CreateDescType, GraphicsBackend::Vulkan>&& other);
	RenderTargetImpl(CreateDescType&& desc);
	virtual ~RenderTargetImpl();

    const auto& getDesc() const { return myDesc; }

protected:

    CreateDescType myDesc = {};
};

template <typename CreateDescType>
RenderTargetImpl<CreateDescType, GraphicsBackend::Vulkan>::RenderTargetImpl(RenderTargetImpl<CreateDescType, GraphicsBackend::Vulkan>&& other)
: BaseType(std::move(other))
, myDesc(std::move(other.myDesc))
{
}

template <typename CreateDescType>
RenderTargetImpl<CreateDescType, GraphicsBackend::Vulkan>::RenderTargetImpl(CreateDescType&& desc)
: BaseType{desc.imageExtent, desc.renderPass}
, myDesc(std::move(desc))
{
    ZoneScopedN("RenderTarget()");

    std::vector<ImageViewHandle<GraphicsBackend::Vulkan>> attachments;

    renderPass = myDesc.renderPass;
    
    for (uint32_t i = 0; i < myDesc.colorImages.size(); i++)
        colorViews.emplace_back(createImageView2D(
            myDesc.deviceContext->getDevice(),
            myDesc.colorImages[i],
            myDesc.colorImageFormat,
            VK_IMAGE_ASPECT_COLOR_BIT));

    attachments.insert(std::end(attachments), std::begin(colorViews), std::end(colorViews));

    if (myDesc.depthImage)
    {
        VkImageAspectFlags depthAspectFlags = VK_IMAGE_ASPECT_DEPTH_BIT;
        if (hasStencilComponent(myDesc.depthImageFormat))
            depthAspectFlags |= VK_IMAGE_ASPECT_STENCIL_BIT;

        depthView = std::make_optional(createImageView2D(
            myDesc.deviceContext->getDevice(),
            myDesc.depthImage,
            myDesc.depthImageFormat,
            depthAspectFlags));

        attachments.push_back(depthView.value());
    }

    frameBuffer = createFramebuffer(
        myDesc.deviceContext->getDevice(),
        myDesc.renderPass,
        attachments.size(),
        attachments.data(),
        myDesc.imageExtent.width,
        myDesc.imageExtent.height,
        1);
}

template <typename CreateDescType>
RenderTargetImpl<CreateDescType, GraphicsBackend::Vulkan>::~RenderTargetImpl()
{
    ZoneScopedN("~RenderTarget()");

    for (const auto& colorView : colorViews)
        vkDestroyImageView(myDesc.deviceContext->getDevice(), colorView, nullptr);

    if (depthView)
        vkDestroyImageView(myDesc.deviceContext->getDevice(), depthView.value(), nullptr);
    
    vkDestroyFramebuffer(myDesc.deviceContext->getDevice(), frameBuffer, nullptr);
}

extern template RenderTargetImpl<RenderTargetCreateDesc<GraphicsBackend::Vulkan>, GraphicsBackend::Vulkan>::RenderTargetImpl(
    RenderTargetImpl<RenderTargetCreateDesc<GraphicsBackend::Vulkan>, GraphicsBackend::Vulkan>&& other);

extern template RenderTargetImpl<RenderTargetCreateDesc<GraphicsBackend::Vulkan>, GraphicsBackend::Vulkan>::RenderTargetImpl(RenderTargetCreateDesc<GraphicsBackend::Vulkan>&& desc);

extern template RenderTargetImpl<RenderTargetCreateDesc<GraphicsBackend::Vulkan>, GraphicsBackend::Vulkan>::~RenderTargetImpl();
