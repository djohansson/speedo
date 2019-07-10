#include "buffer.h"

#include <string>

template <>
Buffer<GraphicsBackend::Vulkan>::Buffer(
    VkDevice device, VmaAllocator allocator,
    BufferCreateDesc<GraphicsBackend::Vulkan>&& desc)
    : myDevice(device)
    , myAllocator(allocator)
    , myDesc(std::move(desc))
{
    assert(myDesc.initialData == nullptr); // todo: implement

    std::tie(myBuffer, myBufferMemory) = createBuffer(
		myAllocator, myDesc.size, myDesc.usageFlags, myDesc.memoryFlags, myDesc.debugName.c_str());

    myDesc.initialData.reset();

    if (myDesc.usageFlags == VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT ||
        myDesc.usageFlags == VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT)
        myBufferView = createBufferView(myDevice, myBuffer, 0, myDesc.format, myDesc.offset, myDesc.range);
}

template <>
Buffer<GraphicsBackend::Vulkan>::~Buffer()
{
    vmaDestroyBuffer(myAllocator, myBuffer, myBufferMemory);
    vkDestroyBufferView(myDevice, myBufferView, nullptr);
}
