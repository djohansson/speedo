#include "swapchain.h"

template <>
SwapchainContext<GraphicsBackend::Vulkan>::SwapchainContext(
    SwapchainCreateDesc<GraphicsBackend::Vulkan>&& desc)
    : myDesc(std::move(desc))
{
    VkSwapchainCreateInfoKHR info = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    info.surface = myDesc.surface;
    info.minImageCount = myDesc.configuration.selectedImageCount;
    info.imageFormat = myDesc.configuration.selectedFormat().format;
    info.imageColorSpace = myDesc.configuration.selectedFormat().colorSpace;
    info.imageExtent = myDesc.imageExtent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.imageSharingMode =
        VK_SHARING_MODE_EXCLUSIVE; // Assume that graphics family == present family
    info.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = myDesc.configuration.selectedPresentMode();
    info.clipped = VK_TRUE;
    info.oldSwapchain = myDesc.previous;

    CHECK_VK(vkCreateSwapchainKHR(myDesc.device, &info, nullptr, &mySwapchain));

    if (myDesc.previous != VK_NULL_HANDLE)
        vkDestroySwapchainKHR(myDesc.device, myDesc.previous, nullptr);
}

template <>
SwapchainContext<GraphicsBackend::Vulkan>::~SwapchainContext()
{
    if (mySwapchain != VK_NULL_HANDLE)
        vkDestroySwapchainKHR(myDesc.device, mySwapchain, nullptr);
}

template <>
const std::vector<ImageHandle<GraphicsBackend::Vulkan>> SwapchainContext<GraphicsBackend::Vulkan>::getColorImages() const
{
    uint32_t imageCount;
    CHECK_VK(vkGetSwapchainImagesKHR(myDesc.device, mySwapchain, &imageCount, nullptr));
    assert(imageCount == myDesc.configuration.selectedImageCount);
    
    std::vector<ImageHandle<GraphicsBackend::Vulkan>> colorImages(imageCount);
    CHECK_VK(vkGetSwapchainImagesKHR(myDesc.device, mySwapchain, &imageCount, colorImages.data()));

    return colorImages;
}
