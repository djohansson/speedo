#include "buffer.h"

#include <string>

template <>
uint32_t Buffer<GraphicsBackend::Vulkan>::ourDebugCount = 0;

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
    CommandContext<GraphicsBackend::Vulkan>& commandContext)
    : myDesc(std::move(desc))
{
    ++ourDebugCount;

    std::tie(myBuffer, myBufferMemory) = createBuffer(
		commandContext.commands(), myDesc.deviceContext->getAllocator(), myDesc.initialData,
        myDesc.size, myDesc.usageFlags, myDesc.memoryFlags, myDesc.debugName.c_str());

    if (myDesc.initialData)
        commandContext.addGarbageCollectCallback([this](uint64_t){
            deleteInitialData(); });
}

template <>
Buffer<GraphicsBackend::Vulkan>::~Buffer()
{
    assert(!myDesc.initialData);

    vmaDestroyBuffer(myDesc.deviceContext->getAllocator(), myBuffer, myBufferMemory);

    --ourDebugCount;
}
