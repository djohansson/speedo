#include "texture.h"

template <>
Texture<GraphicsBackend::Vulkan>::Texture(
    VkDevice device, VkCommandPool commandPool, VkQueue queue, VmaAllocator allocator,
    const std::byte* data, int x, int y, VkFormat format,
    VkImageUsageFlags flags, VkImageAspectFlagBits aspectFlags,
    const char* debugName)
    : device(device)
    , allocator(allocator)
    , format(format)
{
    createDeviceLocalImage2D(
        device, commandPool, queue, allocator,
        data, x, y, format, flags, image, imageMemory, debugName);

    imageView = createImageView2D(device, image, format, aspectFlags);
}

template <>
Texture<GraphicsBackend::Vulkan>::~Texture()
{
    vmaDestroyImage(allocator, image, imageMemory);
    vkDestroyImageView(device, imageView, nullptr);
}
