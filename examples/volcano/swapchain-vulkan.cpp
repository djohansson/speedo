#include "swapchain.h"
#include "vk-utils.h"

#include <core/slang-secure-crt.h>


template <>
SwapchainContext<GraphicsBackend::Vulkan>::SwapchainContext(
    const std::shared_ptr<DeviceContext<GraphicsBackend::Vulkan>>& deviceContext,
    SwapchainCreateDesc<GraphicsBackend::Vulkan>&& desc)
: DeviceResource(deviceContext, desc)
, myDesc(std::move(desc))
{
    ZoneScopedN("SwapchainContext()");

    VkSwapchainCreateInfoKHR info = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    info.surface = getDeviceContext()->getSurface();
    info.minImageCount = getDeviceContext()->getDesc().swapchainConfiguration->imageCount;
    info.imageFormat = getDeviceContext()->getDesc().swapchainConfiguration->surfaceFormat.format;
    info.imageColorSpace = getDeviceContext()->getDesc().swapchainConfiguration->surfaceFormat.colorSpace;
    info.imageExtent = myDesc.imageExtent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = getDeviceContext()->getDesc().swapchainConfiguration->presentMode;
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
    addObject(VK_OBJECT_TYPE_SWAPCHAIN_KHR, reinterpret_cast<uint64_t>(mySwapchain), stringBuffer);

    if (myDesc.previous)
        vkDestroySwapchainKHR(getDeviceContext()->getDevice(), myDesc.previous, nullptr);
}

template <>
SwapchainContext<GraphicsBackend::Vulkan>::~SwapchainContext()
{
    ZoneScopedN("~SwapchainContext()");

    if (mySwapchain)
        vkDestroySwapchainKHR(getDeviceContext()->getDevice(), mySwapchain, nullptr);
}
