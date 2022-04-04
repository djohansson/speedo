#include "swapchain.h"
#include "vk-utils.h"

#include <stb_sprintf.h>

template <>
const std::optional<RenderPassBeginInfo<Vk>>& Swapchain<Vk>::begin(
    CommandBufferHandle<Vk> cmd,
    SubpassContents<Vk> contents)
{
    return myFrames[myFrameIndex].begin(cmd, contents);
}

template <>
void Swapchain<Vk>::end(CommandBufferHandle<Vk> cmd)
{
    return myFrames[myFrameIndex].end(cmd);
}

template <>
const RenderTargetCreateDesc<Vk>& Swapchain<Vk>::getRenderTargetDesc() const
{
    return myDesc;
}

template <>
ImageLayout<Vk> Swapchain<Vk>::getColorImageLayout(uint32_t index) const
{
    return myFrames[myFrameIndex].getColorImageLayout(index);
}

template <>
ImageLayout<Vk> Swapchain<Vk>::getDepthStencilImageLayout() const
{
    return myFrames[myFrameIndex].getDepthStencilImageLayout();
}

template <>
void Swapchain<Vk>::blit(
    CommandBufferHandle<Vk> cmd,
    const IRenderTarget<Vk>& srcRenderTarget,
    const ImageSubresourceLayers<Vk>& srcSubresource,
    uint32_t srcIndex,
    const ImageSubresourceLayers<Vk>& dstSubresource,
    uint32_t dstIndex,
    Filter<Vk> filter)
{
    myFrames[myFrameIndex].blit(cmd, srcRenderTarget, srcSubresource, srcIndex, dstSubresource, dstIndex, filter);
}

template <>
void Swapchain<Vk>::clearSingleAttachment(
    CommandBufferHandle<Vk> cmd,
    const ClearAttachment<Vk>& clearAttachment) const
{
    myFrames[myFrameIndex].clearSingleAttachment(cmd, clearAttachment);
}

template <>
void Swapchain<Vk>::clearAllAttachments(
    CommandBufferHandle<Vk> cmd,
    const ClearColorValue<Vk>& color,
    const ClearDepthStencilValue<Vk>& depthStencil) const
{
    myFrames[myFrameIndex].clearAllAttachments(cmd, color, depthStencil);
}

template <>
void Swapchain<Vk>::clearColor(CommandBufferHandle<Vk> cmd, const ClearColorValue<Vk>& color, uint32_t index)
{
    myFrames[myFrameIndex].clearColor(cmd, color, index);
}

template <>
void Swapchain<Vk>::clearDepthStencil(CommandBufferHandle<Vk> cmd, const ClearDepthStencilValue<Vk>& depthStencil)
{
    myFrames[myFrameIndex].clearDepthStencil(cmd, depthStencil);
}

template <>
void Swapchain<Vk>::transitionColor(CommandBufferHandle<Vk> cmd, ImageLayout<Vk> layout, uint32_t index)
{
    myFrames[myFrameIndex].transitionColor(cmd, layout, index);
}

template <>
void Swapchain<Vk>::transitionDepthStencil(CommandBufferHandle<Vk> cmd, ImageLayout<Vk> layout)
{
    myFrames[myFrameIndex].transitionDepthStencil(cmd, layout);
}

template <>
std::tuple<bool, uint64_t> Swapchain<Vk>::flip()
{
    ZoneScoped;

    static constexpr std::string_view flipFrameStr = "Swapchain::flip";

    const auto& lastFrame = myFrames[myLastFrameIndex];

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

        ZoneName(failedStr, std::ssize(failedStr));

        // todo: print error code
        //ZoneText(failedStr, std::ssize(failedStr));

        return std::make_tuple(false, lastFrame.getLastPresentTimelineValue());
    }

    char flipFrameWithNumberStr[flipFrameStr.size()+2];
    stbsp_sprintf(
        flipFrameWithNumberStr,
        "%.*s%u",
        static_cast<int>(flipFrameStr.size()),
        flipFrameStr.data(),
        myFrameIndex);

    ZoneName(flipFrameWithNumberStr, std::ssize(flipFrameWithNumberStr));

    const auto& frame = myFrames[myFrameIndex];
    
    return std::make_tuple(true, frame.getLastPresentTimelineValue());
}

template <>
QueuePresentInfo<Vk> Swapchain<Vk>::preparePresent(uint64_t timelineValue)
{
    ZoneScopedN("Swapchain::preparePresent");

    myLastFrameIndex = myFrameIndex;

    auto presentInfo = myFrames[myFrameIndex].preparePresent(timelineValue);
    presentInfo.swapchains.push_back(mySwapchain);
    
    return presentInfo;
}

template <>
void Swapchain<Vk>::internalCreateSwapchain(
    const SwapchainConfiguration<Vk>& config,
    SwapchainHandle<Vk> previous)
{
    ZoneScopedN("Swapchain::internalCreateSwapchain");

    myDesc = {config.extent}; // more?

    VkSwapchainCreateInfoKHR info = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    info.surface = mySurface;
    info.minImageCount = config.imageCount;
    info.imageFormat = config.surfaceFormat.format;
    info.imageColorSpace = config.surfaceFormat.colorSpace;
    info.imageExtent = config.extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = config.presentMode;
    info.clipped = VK_TRUE;
    info.oldSwapchain = previous;

    VK_CHECK(vkCreateSwapchainKHR(getDeviceContext()->getDevice(), &info, nullptr, &mySwapchain));

    if (previous)
    {
#if PROFILING_ENABLED
        getDeviceContext()->eraseOwnedObjectHandle(getUid(), reinterpret_cast<uint64_t>(previous)); 
#endif
        vkDestroySwapchainKHR(getDeviceContext()->getDevice(), previous, nullptr);
    }

#if PROFILING_ENABLED
    char stringBuffer[32];
    static constexpr std::string_view swapchainStr = "_Swapchain";
    stbsp_sprintf(
        stringBuffer,
        "%.*s%.*s",
        static_cast<int>(getName().size()),
        getName().c_str(),
        static_cast<int>(swapchainStr.size()),
        swapchainStr.data());
    
    getDeviceContext()->addOwnedObjectHandle(
        getUid(),
        VK_OBJECT_TYPE_SWAPCHAIN_KHR,
        reinterpret_cast<uint64_t>(mySwapchain),
        stringBuffer);
#endif

    uint32_t frameCount = config.imageCount;
    
    assert(frameCount);

    uint32_t imageCount;
    VK_CHECK(vkGetSwapchainImagesKHR(getDeviceContext()->getDevice(), mySwapchain, &imageCount, nullptr));

    assert(imageCount == frameCount);
    
    std::vector<ImageHandle<Vk>> colorImages(imageCount);
    VK_CHECK(vkGetSwapchainImagesKHR(getDeviceContext()->getDevice(), mySwapchain, &imageCount, colorImages.data()));
    
    myFrames.clear();
    myFrames.reserve(frameCount);
    
    for (uint32_t frameIt = 0ul; frameIt < frameCount; frameIt++)
        myFrames.emplace_back(
            Frame<Vk>(
                getDeviceContext(),
                FrameCreateDesc<Vk>{
                    {config.extent,
                    std::make_vector(config.surfaceFormat.format),
                    std::make_vector(VK_IMAGE_LAYOUT_UNDEFINED),
                    std::make_vector(colorImages[frameIt])},
                    frameIt}));

    myLastFrameIndex = frameCount - 1;
}

template <>
Swapchain<Vk>::Swapchain(Swapchain&& other) noexcept
: DeviceObject(std::forward<Swapchain>(other))
, myDesc(std::exchange(other.myDesc, {}))
, mySwapchain(std::exchange(other.mySwapchain, {}))
, myFrames(std::exchange(other.myFrames, {}))
, myFrameIndex(std::exchange(other.myFrameIndex, 0))
, myLastFrameIndex(std::exchange(other.myLastFrameIndex, 0))
{
}

template <>
Swapchain<Vk>::Swapchain(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    const SwapchainConfiguration<Vk>& config,
    SurfaceHandle<Vk>&& surface,
    SwapchainHandle<Vk> previous)
: DeviceObject(deviceContext, {})
, mySurface(std::forward<SurfaceHandle<Vk>>(surface))
{
    ZoneScopedN("Swapchain()");

    internalCreateSwapchain(config, previous);
}

template <>
Swapchain<Vk>::~Swapchain()
{
    ZoneScopedN("~Swapchain()");

    if (mySwapchain)
        vkDestroySwapchainKHR(getDeviceContext()->getDevice(), mySwapchain, nullptr);

    if (mySurface)
        vkDestroySurfaceKHR(getDeviceContext()->getInstanceContext()->getInstance(), mySurface, nullptr);
}

template <>
Swapchain<Vk>& Swapchain<Vk>::operator=(Swapchain&& other) noexcept
{
	DeviceObject::operator=(std::forward<Swapchain>(other));
    myDesc = std::exchange(other.myDesc, {});
    mySwapchain = std::exchange(other.mySwapchain, {});
    myFrames = std::exchange(other.myFrames, {});
    myFrameIndex = std::exchange(other.myFrameIndex, 0);
    myLastFrameIndex = std::exchange(other.myLastFrameIndex, 0);
	return *this;
}

template <>
void Swapchain<Vk>::swap(Swapchain& rhs) noexcept
{
	DeviceObject::swap(rhs);
    std::swap(myDesc, rhs.myDesc);
    std::swap(mySwapchain, rhs.mySwapchain);
    std::swap(myFrames, rhs.myFrames);
    std::swap(myFrameIndex, rhs.myFrameIndex);
    std::swap(myLastFrameIndex, rhs.myLastFrameIndex);
}
