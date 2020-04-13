#include "rendertarget.h"

template <>
RenderTarget<GraphicsBackend::Vulkan>::RenderTarget(RenderTargetDesc<GraphicsBackend::Vulkan>&& desc)
: myDesc(std::move(desc))
{
    std::vector<ImageViewHandle<GraphicsBackend::Vulkan>> attachments;
    
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

        myDepthView = createImageView2D(
            myDesc.deviceContext->getDevice(),
            myDesc.depthImage,
            myDesc.depthImageFormat,
            depthAspectFlags);

        attachments.push_back(myDepthView);
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

template <>
RenderTarget<GraphicsBackend::Vulkan>::~RenderTarget()
{
    for (const auto& colorView : myColorViews)
        vkDestroyImageView(myDesc.deviceContext->getDevice(), colorView, nullptr);

    if (myDepthView)
        vkDestroyImageView(myDesc.deviceContext->getDevice(), myDepthView, nullptr);
    
    vkDestroyFramebuffer(myDesc.deviceContext->getDevice(), myFrameBuffer, nullptr);
}
