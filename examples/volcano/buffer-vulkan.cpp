#include "buffer.h"

#include <string>

template <>
void Buffer<GraphicsBackend::Vulkan>::waitForTransferComplete()
{
    // todo
}

template <>
void Buffer<GraphicsBackend::Vulkan>::deleteInitialData()
{
    waitForTransferComplete();

    vmaDestroyBuffer(myDesc.allocator, myDesc.initialData, myDesc.initialDataMemory);
    myDesc.initialData = 0;
    myDesc.initialDataMemory = 0;
}

template <>
BufferViewHandle<GraphicsBackend::Vulkan>
Buffer<GraphicsBackend::Vulkan>::createView(
    Format<GraphicsBackend::Vulkan> format,
    DeviceSize<GraphicsBackend::Vulkan> offset,
    DeviceSize<GraphicsBackend::Vulkan> range) const
{
    return createBufferView(myDesc.device, myBuffer, 0, format, offset, range);
}

template <>
Buffer<GraphicsBackend::Vulkan>::Buffer(
    BufferCreateDesc<GraphicsBackend::Vulkan>&& desc, CommandBufferHandle<GraphicsBackend::Vulkan> commandBuffer)
    : myDesc(std::move(desc))
{
    std::tie(myBuffer, myBufferMemory) = createBuffer(
		commandBuffer, myDesc.allocator, myDesc.initialData,
        myDesc.size, myDesc.usageFlags, myDesc.memoryFlags, myDesc.debugName.c_str());
}

template <>
Buffer<GraphicsBackend::Vulkan>::~Buffer()
{
    if (myDesc.initialData != 0)
        deleteInitialData();

    vmaDestroyBuffer(myDesc.allocator, myBuffer, myBufferMemory);
}
