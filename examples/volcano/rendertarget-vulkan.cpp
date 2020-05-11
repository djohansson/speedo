#include "rendertarget.h"
#include "vk-utils.h"


template <>
RenderTarget<GraphicsBackend::Vulkan>::RenderTarget(
    RenderTarget<GraphicsBackend::Vulkan>&& other,
    const RenderTargetCreateDesc<GraphicsBackend::Vulkan>* myDescPtr)
: myDeviceContext(other.myDeviceContext)
, myDesc(*myDescPtr)
, myFrameBuffer(other.myFrameBuffer)
, myColorViews(std::move(other.myColorViews))
, myDepthView(std::move(other.myDepthView))
{
}

template <>
RenderTarget<GraphicsBackend::Vulkan>::RenderTarget(
    const std::shared_ptr<DeviceContext<GraphicsBackend::Vulkan>>& deviceContext,
    const RenderTargetCreateDesc<GraphicsBackend::Vulkan>& desc,
    const RenderTargetCreateDesc<GraphicsBackend::Vulkan>* myDescPtr)
: myDeviceContext(deviceContext)
, myDesc(*myDescPtr)
{
    ZoneScopedN("RenderTarget()");

    std::vector<ImageViewHandle<GraphicsBackend::Vulkan>> attachments;
    
    for (uint32_t i = 0; i < desc.colorImages.size(); i++)
        myColorViews.emplace_back(createImageView2D(
            myDeviceContext->getDevice(),
            desc.colorImages[i],
            desc.colorImageFormat,
            VK_IMAGE_ASPECT_COLOR_BIT));

    attachments.insert(std::end(attachments), std::begin(myColorViews), std::end(myColorViews));

    if (desc.depthImage)
    {
        VkImageAspectFlags depthAspectFlags = VK_IMAGE_ASPECT_DEPTH_BIT;
        if (hasStencilComponent(desc.depthImageFormat))
            depthAspectFlags |= VK_IMAGE_ASPECT_STENCIL_BIT;

        myDepthView = std::make_optional(createImageView2D(
            myDeviceContext->getDevice(),
            desc.depthImage,
            desc.depthImageFormat,
            depthAspectFlags));

        attachments.push_back(myDepthView.value());
    }

    myFrameBuffer = createFramebuffer(
        myDeviceContext->getDevice(),
        desc.renderPass,
        attachments.size(),
        attachments.data(),
        desc.imageExtent.width,
        desc.imageExtent.height,
        1);
}

template <>
RenderTarget<GraphicsBackend::Vulkan>::~RenderTarget()
{
    ZoneScopedN("~RenderTarget()");

    for (const auto& colorView : myColorViews)
        vkDestroyImageView(myDeviceContext->getDevice(), colorView, nullptr);

    if (myDepthView)
        vkDestroyImageView(myDeviceContext->getDevice(), myDepthView.value(), nullptr);
    
    vkDestroyFramebuffer(myDeviceContext->getDevice(), myFrameBuffer, nullptr);
}

template RenderTargetImpl<RenderTargetCreateDesc<GraphicsBackend::Vulkan>, GraphicsBackend::Vulkan>::RenderTargetImpl(
    RenderTargetImpl<RenderTargetCreateDesc<GraphicsBackend::Vulkan>, GraphicsBackend::Vulkan>&& other);

template RenderTargetImpl<RenderTargetCreateDesc<GraphicsBackend::Vulkan>, GraphicsBackend::Vulkan>::RenderTargetImpl(
    const std::shared_ptr<DeviceContext<GraphicsBackend::Vulkan>>& deviceContext,
    RenderTargetCreateDesc<GraphicsBackend::Vulkan>&& desc);

template RenderTargetImpl<RenderTargetCreateDesc<GraphicsBackend::Vulkan>, GraphicsBackend::Vulkan>::~RenderTargetImpl();
