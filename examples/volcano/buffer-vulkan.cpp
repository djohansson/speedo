#include "buffer.h"

#include <string>

template <>
Buffer<GraphicsBackend::Vulkan>::Buffer(
    VkDevice device, VmaAllocator allocator,
    BufferCreateDesc<GraphicsBackend::Vulkan>&& desc)
    : device(device)
    , allocator(allocator)
    , desc(std::move(desc))
{
    assert(desc.initialData == nullptr); // todo: implement

    std::tie(buffer, bufferMemory) = createBuffer(
		allocator, desc.size, desc.usageFlags, desc.memoryFlags, desc.debugName);

    if (desc.usageFlags == VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT ||
        desc.usageFlags == VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT)
        bufferView = createBufferView(device, buffer, 0, desc.format, desc.offset, desc.range);
}

template <>
Buffer<GraphicsBackend::Vulkan>::~Buffer()
{
    vmaDestroyBuffer(allocator, buffer, bufferMemory);
    vkDestroyBufferView(device, bufferView, nullptr);
}
