#include "rendertarget.h"

template <>
RenderTarget<GraphicsBackend::Vulkan>::RenderTarget(RenderTargetDesc<GraphicsBackend::Vulkan>&& desc)
: myRenderTargetDesc(std::move(desc))
{
    std::vector<ImageViewHandle<GraphicsBackend::Vulkan>> attachments;
    
    for (uint32_t i = 0; i < myRenderTargetDesc.colorImageCount; i++)
        myColorViews.emplace_back(createImageView2D(
            myRenderTargetDesc.deviceContext->getDevice(),
            myRenderTargetDesc.colorImages[i],
            myRenderTargetDesc.colorImageFormat,
            VK_IMAGE_ASPECT_COLOR_BIT));

    attachments.insert(std::end(attachments), std::begin(myColorViews), std::end(myColorViews));

    if (myRenderTargetDesc.depthImage)
    {
        VkImageAspectFlags depthAspectFlags = VK_IMAGE_ASPECT_DEPTH_BIT;
        if (hasStencilComponent(myRenderTargetDesc.depthImageFormat))
            depthAspectFlags |= VK_IMAGE_ASPECT_STENCIL_BIT;

        myDepthView = createImageView2D(
            myRenderTargetDesc.deviceContext->getDevice(),
            myRenderTargetDesc.depthImage,
            myRenderTargetDesc.depthImageFormat,
            depthAspectFlags);

        attachments.push_back(myDepthView);
    }

    myFrameBuffer = createFramebuffer(
        myRenderTargetDesc.deviceContext->getDevice(),
        myRenderTargetDesc.renderPass,
        attachments.size(),
        attachments.data(),
        myRenderTargetDesc.imageExtent.width,
        myRenderTargetDesc.imageExtent.height,
        1);
}

template <>
RenderTarget<GraphicsBackend::Vulkan>::~RenderTarget()
{
    for (const auto& colorView : myColorViews)
        vkDestroyImageView(myRenderTargetDesc.deviceContext->getDevice(), colorView, nullptr);

    if (myDepthView)
        vkDestroyImageView(myRenderTargetDesc.deviceContext->getDevice(), myDepthView, nullptr);
    
    vkDestroyFramebuffer(myRenderTargetDesc.deviceContext->getDevice(), myFrameBuffer, nullptr);
}
