#include "swapchain.h"
#include "vk-utils.h"

#include <Tracy.hpp>


template <>
SwapchainContext<GraphicsBackend::Vulkan>::SwapchainContext(
    SwapchainCreateDesc<GraphicsBackend::Vulkan>&& desc)
    : myDesc(std::move(desc))
{
    ZoneScopedN("SwapchainContext()");

    VkSwapchainCreateInfoKHR info = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    info.surface = myDesc.deviceContext->getDesc().instanceContext->getSurface();
    info.minImageCount = myDesc.deviceContext->getDesc().swapchainConfiguration->imageCount;
    info.imageFormat = myDesc.deviceContext->getDesc().swapchainConfiguration->surfaceFormat.format;
    info.imageColorSpace = myDesc.deviceContext->getDesc().swapchainConfiguration->surfaceFormat.colorSpace;
    info.imageExtent = myDesc.imageExtent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = myDesc.deviceContext->getDesc().swapchainConfiguration->presentMode;
    info.clipped = VK_TRUE;
    info.oldSwapchain = myDesc.previous;

    CHECK_VK(vkCreateSwapchainKHR(myDesc.deviceContext->getDevice(), &info, nullptr, &mySwapchain));

    if (myDesc.previous != VK_NULL_HANDLE)
        vkDestroySwapchainKHR(myDesc.deviceContext->getDevice(), myDesc.previous, nullptr);
}

template <>
SwapchainContext<GraphicsBackend::Vulkan>::~SwapchainContext()
{
    ZoneScopedN("~SwapchainContext()");

    if (mySwapchain != VK_NULL_HANDLE)
        vkDestroySwapchainKHR(myDesc.deviceContext->getDevice(), mySwapchain, nullptr);
}
