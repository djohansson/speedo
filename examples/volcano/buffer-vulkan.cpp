#include "buffer.h"

#include <string>

template <>
Buffer<GraphicsBackend::Vulkan>::Buffer(
    VkDevice device, VkCommandPool commandPool, VkQueue queue, VmaAllocator allocator,
    BufferCreateDesc<GraphicsBackend::Vulkan>&& desc)
    : myDevice(device)
    , myAllocator(allocator)
    , myDesc(std::move(desc))
{
    std::tie(myBuffer, myBufferMemory) = createBuffer(
		myDevice, commandPool, queue, myAllocator, myDesc.initialData, myDesc.size, myDesc.usageFlags, myDesc.memoryFlags, myDesc.debugName.c_str());

    vmaDestroyBuffer(myAllocator, myDesc.initialData, myDesc.initialDataMemory);
}

template <>
Buffer<GraphicsBackend::Vulkan>::~Buffer()
{
    vmaDestroyBuffer(myAllocator, myBuffer, myBufferMemory);
}

template <>
BufferViewHandle<GraphicsBackend::Vulkan> Buffer<GraphicsBackend::Vulkan>::createView(
    Format<GraphicsBackend::Vulkan> format, DeviceSize<GraphicsBackend::Vulkan> offset, DeviceSize<GraphicsBackend::Vulkan> range) const
{
    return createBufferView(myDevice, myBuffer, 0, format, offset, range);
}
