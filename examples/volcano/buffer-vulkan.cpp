#include "buffer.h"

#include <string>

template <>
void Buffer<GraphicsBackend::Vulkan>::deleteInitialData()
{
    vmaDestroyBuffer(myDesc.deviceContext->getAllocator(), myDesc.initialData, myDesc.initialDataMemory);
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
    return createBufferView(myDesc.deviceContext->getDevice(), myBuffer, 0, format, offset, range);
}

template <>
Buffer<GraphicsBackend::Vulkan>::Buffer(
    BufferDesc<GraphicsBackend::Vulkan>&& desc,
    CommandContext<GraphicsBackend::Vulkan>& commands)
    : myDesc(std::move(desc))
{
    std::tie(myBuffer, myBufferMemory) = createBuffer(
		commands.getCommandBuffer(), myDesc.deviceContext->getAllocator(), myDesc.initialData,
        myDesc.size, myDesc.usageFlags, myDesc.memoryFlags, myDesc.debugName.c_str());

    commands.addSyncCallback([this]{ deleteInitialData(); });
}

template <>
Buffer<GraphicsBackend::Vulkan>::~Buffer()
{
    assert(!myDesc.initialData);

    vmaDestroyBuffer(myDesc.deviceContext->getAllocator(), myBuffer, myBufferMemory);
}
