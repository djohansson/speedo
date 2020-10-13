#include "swapchain.h"
#include "vk-utils.h"

#include <stb_sprintf.h>

template <>
Swapchain<Vk>::Swapchain(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    SwapchainCreateDesc<Vk>&& desc)
: DeviceResource(deviceContext, desc)
, myDesc(std::move(desc))
{
    ZoneScopedN("Swapchain()");

    const auto& config = deviceContext->getDesc().swapchainConfig;

    VkSwapchainCreateInfoKHR info = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    info.surface = deviceContext->getSurface();
    info.minImageCount = config->imageCount;
    info.imageFormat = config->surfaceFormat.format;
    info.imageColorSpace = config->surfaceFormat.colorSpace;
    info.imageExtent = myDesc.extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = deviceContext->getDesc().swapchainConfig->presentMode;
    info.clipped = VK_TRUE;
    info.oldSwapchain = myDesc.previous;

    VK_CHECK(vkCreateSwapchainKHR(deviceContext->getDevice(), &info, nullptr, &mySwapchain));

    char stringBuffer[32];
    static constexpr std::string_view swapchainStr = "_Swapchain";
    stbsp_sprintf(
        stringBuffer,
        "%.*s%.*s",
        getName().size(),
        getName().c_str(),
        static_cast<int>(swapchainStr.size()),
        swapchainStr.data());
    
    deviceContext->addOwnedObject(
        getId(),
        VK_OBJECT_TYPE_SWAPCHAIN_KHR,
        reinterpret_cast<uint64_t>(mySwapchain),
        stringBuffer);

    uint32_t frameCount = config->imageCount;
    
    assert(frameCount);

    uint32_t imageCount;
    VK_CHECK(vkGetSwapchainImagesKHR(deviceContext->getDevice(), mySwapchain, &imageCount, nullptr));

    assert(imageCount == frameCount);
    
    std::vector<ImageHandle<Vk>> colorImages(imageCount);
    VK_CHECK(vkGetSwapchainImagesKHR(deviceContext->getDevice(), mySwapchain, &imageCount, colorImages.data()));
    
    myFrames.reserve(frameCount);
    
    for (uint32_t frameIt = 0; frameIt < frameCount; frameIt++)
        myFrames.emplace_back(std::make_unique<Frame<Vk>>(
            deviceContext,
            FrameCreateDesc<Vk>{
                {{"Frame"},
                myDesc.extent,
                make_vector(config->surfaceFormat.format),
                make_vector(VK_IMAGE_LAYOUT_UNDEFINED),
                make_vector(colorImages[frameIt])},
                frameIt}));

    myLastFrameIndex = frameCount - 1;
}

template <>
const std::optional<RenderPassBeginInfo<Vk>>& Swapchain<Vk>::begin(
    CommandBufferHandle<Vk> cmd,
    SubpassContents<Vk> contents)
{
    return myFrames[myFrameIndex]->begin(cmd, contents);
}

template <>
void Swapchain<Vk>::end(CommandBufferHandle<Vk> cmd)
{
    return myFrames[myFrameIndex]->end(cmd);
}

template <>
const RenderTargetCreateDesc<Vk>& Swapchain<Vk>::getRenderTargetDesc() const
{
    return myDesc;
}

template <>
ImageLayout<Vk> Swapchain<Vk>::getColorImageLayout(uint32_t index) const
{
    return myFrames[myFrameIndex]->getColorImageLayout(index);
}

template <>
ImageLayout<Vk> Swapchain<Vk>::getDepthStencilImageLayout() const
{
    return myFrames[myFrameIndex]->getDepthStencilImageLayout();
}

template <>
void Swapchain<Vk>::blit(
    CommandBufferHandle<Vk> cmd,
    const std::shared_ptr<IRenderTarget<Vk>>& srcRenderTarget,
    const ImageSubresourceLayers<Vk>& srcSubresource,
    uint32_t srcIndex,
    const ImageSubresourceLayers<Vk>& dstSubresource,
    uint32_t dstIndex,
    Filter<Vk> filter)
{
    myFrames[myFrameIndex]->blit(cmd, srcRenderTarget, srcSubresource, srcIndex, dstSubresource, dstIndex, filter);
}

template <>
void Swapchain<Vk>::clearSingleAttachment(
    CommandBufferHandle<Vk> cmd,
    const ClearAttachment<Vk>& clearAttachment) const
{
    myFrames[myFrameIndex]->clearSingleAttachment(cmd, clearAttachment);
}

template <>
void Swapchain<Vk>::clearAllAttachments(
    CommandBufferHandle<Vk> cmd,
    const ClearColorValue<Vk>& color,
    const ClearDepthStencilValue<Vk>& depthStencil) const
{
    myFrames[myFrameIndex]->clearAllAttachments(cmd, color, depthStencil);
}

template <>
void Swapchain<Vk>::clearColor(CommandBufferHandle<Vk> cmd, const ClearColorValue<Vk>& color, uint32_t index)
{
    myFrames[myFrameIndex]->clearColor(cmd, color, index);
}

template <>
void Swapchain<Vk>::clearDepthStencil(CommandBufferHandle<Vk> cmd, const ClearDepthStencilValue<Vk>& depthStencil)
{
    myFrames[myFrameIndex]->clearDepthStencil(cmd, depthStencil);
}

template <>
void Swapchain<Vk>::transitionColor(CommandBufferHandle<Vk> cmd, ImageLayout<Vk> layout, uint32_t index)
{
    myFrames[myFrameIndex]->transitionColor(cmd, layout, index);
}

template <>
void Swapchain<Vk>::transitionDepthStencil(CommandBufferHandle<Vk> cmd, ImageLayout<Vk> layout)
{
    myFrames[myFrameIndex]->transitionDepthStencil(cmd, layout);
}

template <>
std::tuple<SemaphoreHandle<Vk>, SemaphoreHandle<Vk>> Swapchain<Vk>::getFrameSyncSemaphores() const
{
    const auto& lastFrame = *myFrames[myLastFrameIndex];
    const auto& frame = *myFrames[myFrameIndex];

    return std::make_tuple(lastFrame.getNewImageAcquiredSemaphore(), frame.getRenderCompleteSemaphore());
}

template <>
std::tuple<bool, uint64_t> Swapchain<Vk>::flip()
{
    ZoneScoped;

    static constexpr std::string_view flipFrameStr = "Swapchain::flip";

    const auto& lastFrame = *myFrames[myLastFrameIndex];

    auto flipResult = checkFlipOrPresentResult(
        vkAcquireNextImageKHR(
            getDeviceContext()->getDevice(),
            mySwapchain,
            UINT64_MAX,
            lastFrame.getNewImageAcquiredSemaphore(),
            0,
            &myFrameIndex));

    if (flipResult != VK_SUCCESS)
    {
        //assert(lastFrameIndex == frameIndex); // just check that vkAcquireNextImageKHR is not messing up things

        static constexpr std::string_view errorStr = " - ERROR: vkAcquireNextImageKHR failed";

        char failedStr[flipFrameStr.size() + errorStr.size() + 1];
        stbsp_sprintf(
            failedStr,
             "%.*s%.*s",
            static_cast<int>(flipFrameStr.size()),
            flipFrameStr.data(),
            static_cast<int>(errorStr.size()),
            errorStr.data());

        ZoneName(failedStr, sizeof_array(failedStr));

        // todo: print error code
        //ZoneText(failedStr, sizeof_array(failedStr));

        return std::make_tuple(false, lastFrame.getLastPresentTimelineValue());
    }

    char flipFrameWithNumberStr[flipFrameStr.size()+2];
    stbsp_sprintf(
        flipFrameWithNumberStr,
        "%.*s%u",
        static_cast<int>(flipFrameStr.size()),
        flipFrameStr.data(),
        myFrameIndex);

    ZoneName(flipFrameWithNumberStr, sizeof_array(flipFrameWithNumberStr));

    const auto& frame = *myFrames[myFrameIndex];
    
    return std::make_tuple(true, frame.getLastPresentTimelineValue());
}

template <>
QueuePresentInfo<Vk> Swapchain<Vk>::preparePresent(uint64_t timelineValue)
{
    myLastFrameIndex = myFrameIndex;

    auto presentInfo = myFrames[myFrameIndex]->preparePresent(timelineValue);
    presentInfo.swapchains.push_back(mySwapchain);
    
    return presentInfo;
}

template <>
Swapchain<Vk>::~Swapchain()
{
    ZoneScopedN("~Swapchain()");

    if (mySwapchain)
        vkDestroySwapchainKHR(getDeviceContext()->getDevice(), mySwapchain, nullptr);
}
