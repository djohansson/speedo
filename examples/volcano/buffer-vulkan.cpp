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
