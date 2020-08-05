#include "swapchain.h"
#include "vk-utils.h"

#include <core/slang-secure-crt.h>

template <>
SwapchainContext<Vk>::SwapchainContext(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    SwapchainCreateDesc<Vk>&& desc)
: DeviceResource(deviceContext, desc)
, myDesc(std::move(desc))
{
    ZoneScopedN("SwapchainContext()");

    const auto& config = deviceContext->getDesc().swapchainConfig;

    VkSwapchainCreateInfoKHR info = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    info.surface = deviceContext->getSurface();
    info.minImageCount = config->imageCount;
    info.imageFormat = config->surfaceFormat.format;
    info.imageColorSpace = config->surfaceFormat.colorSpace;
    info.imageExtent = myDesc.imageExtent;
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
    sprintf_s(
        stringBuffer,
        sizeof(stringBuffer),
        "%.*s%.*s",
        getName().size(),
        getName().c_str(),
        static_cast<int>(swapchainStr.size()),
        swapchainStr.data());
    
    deviceContext->addOwnedObject(
        this,
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
    
    myFrames.clear();
    myFrames.reserve(frameCount);
    
    for (uint32_t frameIt = 0; frameIt < frameCount; frameIt++)
        myFrames.emplace_back(std::make_unique<Frame<Vk>>(
            deviceContext,
            FrameCreateDesc<Vk>{
                {{"Frame"},
                myDesc.imageExtent,
                make_vector(config->surfaceFormat.format),
                make_vector(VK_IMAGE_LAYOUT_UNDEFINED),
                make_vector(colorImages[frameIt])},
                frameIt}));

    myLastFrameIndex = frameCount - 1;
}

template <>
std::tuple<bool, uint32_t, uint64_t> SwapchainContext<Vk>::flip() const
{
    ZoneScoped;

    static constexpr std::string_view flipFrameStr = "flip";

    const auto& lastFrame = *myFrames[myLastFrameIndex];

    uint32_t frameIndex;
    auto flipResult = checkFlipOrPresentResult(
        vkAcquireNextImageKHR(
            getDeviceContext()->getDevice(),
            mySwapchain,
            UINT64_MAX,
            lastFrame.getNewImageAcquiredSemaphore(),
            0,
            &frameIndex));

    if (flipResult != VK_SUCCESS)
    {
        //assert(lastFrameIndex == frameIndex); // just check that vkAcquireNextImageKHR is not messing up things

        static constexpr std::string_view errorStr = " - ERROR: vkAcquireNextImageKHR failed";

        char failedStr[flipFrameStr.size() + errorStr.size() + 1];
        sprintf_s(failedStr, sizeof(failedStr), "%.*s%.*s",
            static_cast<int>(flipFrameStr.size()), flipFrameStr.data(),
            static_cast<int>(errorStr.size()), errorStr.data());

        ZoneName(failedStr, sizeof_array(failedStr));

        // todo: print error code
        //ZoneText(failedStr, sizeof_array(failedStr));

        return std::make_tuple(false, myLastFrameIndex, lastFrame.getLastSubmitTimelineValue());
    }

    char flipFrameWithNumberStr[flipFrameStr.size()+2];
    sprintf_s(flipFrameWithNumberStr, sizeof(flipFrameWithNumberStr), "%.*s%u",
        static_cast<int>(flipFrameStr.size()), flipFrameStr.data(), frameIndex);

    ZoneName(flipFrameWithNumberStr, sizeof_array(flipFrameWithNumberStr));

    const auto& frame = *myFrames[frameIndex];
    
    return std::make_tuple(true, frameIndex, frame.getLastSubmitTimelineValue());
}

template <>
uint64_t SwapchainContext<Vk>::submit(
    const std::shared_ptr<CommandContext<Vk>>& commandContext,
    uint32_t frameIndex,
    uint64_t waitTimelineValue)
{
    const auto& lastFrame = *myFrames[myLastFrameIndex];
    auto& frame = *myFrames[frameIndex];
    return frame.submit(
        commandContext,
        lastFrame.getNewImageAcquiredSemaphore(),
        waitTimelineValue);
}

template <>
void SwapchainContext<Vk>::present(uint32_t frameIndex)
{
    ZoneScopedN("present");

    const auto& frame = *myFrames[frameIndex];

    VkPresentInfoKHR info = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &frame.getRenderCompleteSemaphore();
    info.swapchainCount = 1;
    info.pSwapchains = &mySwapchain;
    info.pImageIndices = &frameIndex;
    checkFlipOrPresentResult(vkQueuePresentKHR(getDeviceContext()->getGraphicsQueue(), &info));

    myLastFrameIndex = frameIndex;
}

template <>
SwapchainContext<Vk>::~SwapchainContext()
{
    ZoneScopedN("~SwapchainContext()");

    if (mySwapchain)
        vkDestroySwapchainKHR(getDeviceContext()->getDevice(), mySwapchain, nullptr);
}
