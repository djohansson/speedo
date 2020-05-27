#include "rendertarget.h"
#include "vk-utils.h"

#include <core/slang-secure-crt.h>


template <>
RenderTarget<GraphicsBackend::Vulkan>::RenderTarget(
    RenderTarget<GraphicsBackend::Vulkan>&& other,
    const RenderTargetCreateDesc<GraphicsBackend::Vulkan>* myDescPtr)
: DeviceResource<GraphicsBackend::Vulkan>(std::move(other))
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
: DeviceResource<GraphicsBackend::Vulkan>(deviceContext, desc)
, myDesc(*myDescPtr)
{
    ZoneScopedN("RenderTarget()");

    std::vector<ImageViewHandle<GraphicsBackend::Vulkan>> attachments;

    char stringBuffer[32];
    static constexpr std::string_view colorImageViewStr = "_ColorImageView";
    static constexpr std::string_view depthImageViewStr = "_DepthImageView";
    static constexpr std::string_view framebufferStr = "_FrameBuffer";

    for (uint32_t colorImageIt = 0; colorImageIt < desc.colorImages.size(); colorImageIt++)
    {
        myColorViews.emplace_back(createImageView2D(
            getDeviceContext()->getDevice(),
            desc.colorImages[colorImageIt],
            desc.colorImageFormat,
            VK_IMAGE_ASPECT_COLOR_BIT));

        sprintf_s(
            stringBuffer,
            sizeof(stringBuffer),
            "%.*s%.*s%.*u",
            getName().size(),
            getName().c_str(),
            static_cast<int>(colorImageViewStr.size()),
            colorImageViewStr.data(),
            1,
            colorImageIt);

        addObject(
            VK_OBJECT_TYPE_IMAGE_VIEW,
            reinterpret_cast<uint64_t>(myColorViews.back()),
            stringBuffer);
    }

    attachments.insert(std::end(attachments), std::begin(myColorViews), std::end(myColorViews));

    if (desc.depthImage)
    {
        VkImageAspectFlags depthAspectFlags = VK_IMAGE_ASPECT_DEPTH_BIT;
        if (hasStencilComponent(desc.depthImageFormat))
            depthAspectFlags |= VK_IMAGE_ASPECT_STENCIL_BIT;

        myDepthView = std::make_optional(createImageView2D(
            getDeviceContext()->getDevice(),
            desc.depthImage,
            desc.depthImageFormat,
            depthAspectFlags));

        sprintf_s(
            stringBuffer,
            sizeof(stringBuffer),
            "%.*s%.*s",
            getName().size(),
            getName().c_str(),
            static_cast<int>(depthImageViewStr.size()),
            depthImageViewStr.data());

        addObject(
            VK_OBJECT_TYPE_IMAGE_VIEW,
            reinterpret_cast<uint64_t>(myDepthView.value()),
            stringBuffer);

        attachments.push_back(myDepthView.value());
    }

    myFrameBuffer = createFramebuffer(
        getDeviceContext()->getDevice(),
        desc.renderPass,
        attachments.size(),
        attachments.data(),
        desc.imageExtent.width,
        desc.imageExtent.height,
        1);

    sprintf_s(
        stringBuffer,
        sizeof(stringBuffer),
        "%.*s%.*s",
        getName().size(),
        getName().c_str(),
        static_cast<int>(framebufferStr.size()),
        getName().c_str(),
        framebufferStr.data());

    addObject(
        VK_OBJECT_TYPE_FRAMEBUFFER,
        reinterpret_cast<uint64_t>(myFrameBuffer),
        stringBuffer);
}

template <>
RenderTarget<GraphicsBackend::Vulkan>::~RenderTarget()
{
    ZoneScopedN("~RenderTarget()");

    for (const auto& colorView : myColorViews)
        vkDestroyImageView(getDeviceContext()->getDevice(), colorView, nullptr);

    if (myDepthView)
        vkDestroyImageView(getDeviceContext()->getDevice(), myDepthView.value(), nullptr);
    
    vkDestroyFramebuffer(getDeviceContext()->getDevice(), myFrameBuffer, nullptr);
}

template RenderTargetImpl<RenderTargetCreateDesc<GraphicsBackend::Vulkan>, GraphicsBackend::Vulkan>::RenderTargetImpl(
    RenderTargetImpl<RenderTargetCreateDesc<GraphicsBackend::Vulkan>, GraphicsBackend::Vulkan>&& other);

template RenderTargetImpl<RenderTargetCreateDesc<GraphicsBackend::Vulkan>, GraphicsBackend::Vulkan>::RenderTargetImpl(
    const std::shared_ptr<DeviceContext<GraphicsBackend::Vulkan>>& deviceContext,
    RenderTargetCreateDesc<GraphicsBackend::Vulkan>&& desc);

template RenderTargetImpl<RenderTargetCreateDesc<GraphicsBackend::Vulkan>, GraphicsBackend::Vulkan>::~RenderTargetImpl();
