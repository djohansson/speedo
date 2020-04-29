#include "swapchain.h"
#include "vk-utils.h"

#include <Tracy.hpp>


template <>
SwapchainContext<GraphicsBackend::Vulkan>::SwapchainContext(
    SwapchainDesc<GraphicsBackend::Vulkan>&& desc)
    : mySwapchainDesc(std::move(desc))
{
    ZoneScopedN("SwapchainContext()");

    VkSwapchainCreateInfoKHR info = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    info.surface = mySwapchainDesc.deviceContext->getDeviceDesc().instanceContext->getSurface();
    info.minImageCount = mySwapchainDesc.deviceContext->getSwapchainConfiguration().selectedImageCount;
    info.imageFormat = mySwapchainDesc.deviceContext->getSwapchainConfiguration().selectedFormat.format;
    info.imageColorSpace = mySwapchainDesc.deviceContext->getSwapchainConfiguration().selectedFormat.colorSpace;
    info.imageExtent = mySwapchainDesc.imageExtent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.imageSharingMode =
        VK_SHARING_MODE_EXCLUSIVE; // Assume that graphics family == present family
    info.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = mySwapchainDesc.deviceContext->getSwapchainConfiguration().selectedPresentMode;
    info.clipped = VK_TRUE;
    info.oldSwapchain = mySwapchainDesc.previous;

    CHECK_VK(vkCreateSwapchainKHR(mySwapchainDesc.deviceContext->getDevice(), &info, nullptr, &mySwapchain));

    if (mySwapchainDesc.previous != VK_NULL_HANDLE)
        vkDestroySwapchainKHR(mySwapchainDesc.deviceContext->getDevice(), mySwapchainDesc.previous, nullptr);
}

template <>
SwapchainContext<GraphicsBackend::Vulkan>::~SwapchainContext()
{
    ZoneScopedN("~SwapchainContext()");

    if (mySwapchain != VK_NULL_HANDLE)
        vkDestroySwapchainKHR(mySwapchainDesc.deviceContext->getDevice(), mySwapchain, nullptr);
}
