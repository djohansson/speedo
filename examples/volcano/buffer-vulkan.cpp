#include "buffer.h"

#include <string>

template <>
Buffer<GraphicsBackend::Vulkan>::Buffer(
    VkDevice device, VmaAllocator allocator,
    const BufferData<GraphicsBackend::Vulkan>& data)
    : device(device)
    , allocator(allocator)
    , format(data.format)
{
    assert(data.bufferData.get() == nullptr); // todo: implement constructor for this case

    std::tie(buffer, bufferMemory) = createBuffer(
			allocator, data.bufferSize, data.usageFlags, data.memoryFlags, data.debugName.c_str());

    if (data.usageFlags == VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT ||
        data.usageFlags == VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT)
        bufferView = createBufferView(device, buffer, 0, data.format, data.offset, data.range);
}

template <>
Buffer<GraphicsBackend::Vulkan>::~Buffer()
{
    vmaDestroyBuffer(allocator, buffer, bufferMemory);
    vkDestroyBufferView(device, bufferView, nullptr);
}
