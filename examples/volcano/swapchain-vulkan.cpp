#include "swapchain.h"
#include "vk-utils.h"


template <>
SwapchainContext<GraphicsBackend::Vulkan>::SwapchainContext(
    const std::shared_ptr<DeviceContext<GraphicsBackend::Vulkan>>& deviceContext,
    SwapchainCreateDesc<GraphicsBackend::Vulkan>&& desc)
    : myDeviceContext(deviceContext)
    , myDesc(std::move(desc))
{
    ZoneScopedN("SwapchainContext()");

    VkSwapchainCreateInfoKHR info = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    info.surface = myDeviceContext->getSurface();
    info.minImageCount = myDeviceContext->getDesc().swapchainConfiguration->imageCount;
    info.imageFormat = myDeviceContext->getDesc().swapchainConfiguration->surfaceFormat.format;
    info.imageColorSpace = myDeviceContext->getDesc().swapchainConfiguration->surfaceFormat.colorSpace;
    info.imageExtent = myDesc.imageExtent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = myDeviceContext->getDesc().swapchainConfiguration->presentMode;
    info.clipped = VK_TRUE;
    info.oldSwapchain = myDesc.previous;

    VK_CHECK(vkCreateSwapchainKHR(myDeviceContext->getDevice(), &info, nullptr, &mySwapchain));

    if (myDesc.previous)
        vkDestroySwapchainKHR(myDeviceContext->getDevice(), myDesc.previous, nullptr);
}

template <>
SwapchainContext<GraphicsBackend::Vulkan>::~SwapchainContext()
{
    ZoneScopedN("~SwapchainContext()");

    if (mySwapchain)
        vkDestroySwapchainKHR(myDeviceContext->getDevice(), mySwapchain, nullptr);
}
