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

    VkSwapchainCreateInfoKHR info = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    info.surface = getDeviceContext()->getSurface();
    info.minImageCount = getDeviceContext()->getDesc().swapchainConfig->imageCount;
    info.imageFormat = getDeviceContext()->getDesc().swapchainConfig->surfaceFormat.format;
    info.imageColorSpace = getDeviceContext()->getDesc().swapchainConfig->surfaceFormat.colorSpace;
    info.imageExtent = myDesc.imageExtent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = getDeviceContext()->getDesc().swapchainConfig->presentMode;
    info.clipped = VK_TRUE;
    info.oldSwapchain = myDesc.previous;

    VK_CHECK(vkCreateSwapchainKHR(getDeviceContext()->getDevice(), &info, nullptr, &mySwapchain));

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
}

template <>
SwapchainContext<Vk>::~SwapchainContext()
{
    ZoneScopedN("~SwapchainContext()");

    if (mySwapchain)
        vkDestroySwapchainKHR(getDeviceContext()->getDevice(), mySwapchain, nullptr);
}
