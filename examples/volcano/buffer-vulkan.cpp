#include "buffer.h"
#include "vk-utils.h"

#include <string>

#include <Tracy.hpp>

template <>
uint32_t Buffer<GraphicsBackend::Vulkan>::ourDebugCount = 0;

template <>
void Buffer<GraphicsBackend::Vulkan>::deleteInitialData()
{
    vmaDestroyBuffer(myBufferDesc.deviceContext->getAllocator(), myBufferDesc.initialData, myBufferDesc.initialDataMemory);
    myBufferDesc.initialData = 0;
    myBufferDesc.initialDataMemory = 0;
}

template <>
BufferViewHandle<GraphicsBackend::Vulkan>
Buffer<GraphicsBackend::Vulkan>::createView(
    Format<GraphicsBackend::Vulkan> format,
    DeviceSize<GraphicsBackend::Vulkan> offset,
    DeviceSize<GraphicsBackend::Vulkan> range) const
{
    return createBufferView(myBufferDesc.deviceContext->getDevice(), myBuffer, 0, format, offset, range);
}

template <>
Buffer<GraphicsBackend::Vulkan>::Buffer(
    BufferDesc<GraphicsBackend::Vulkan>&& desc,
    CommandContext<GraphicsBackend::Vulkan>& commandContext)
    : myBufferDesc(std::move(desc))
{
    ZoneScopedN("Buffer()");

    ++ourDebugCount;

    std::tie(myBuffer, myBufferMemory) = createBuffer(
		commandContext.commands(), myBufferDesc.deviceContext->getAllocator(), myBufferDesc.initialData,
        myBufferDesc.size, myBufferDesc.usageFlags, myBufferDesc.memoryFlags, myBufferDesc.debugName.c_str());

    if (myBufferDesc.initialData)
        commandContext.addGarbageCollectCallback([this](uint64_t){
            deleteInitialData(); });
}

template <>
Buffer<GraphicsBackend::Vulkan>::~Buffer()
{
    ZoneScopedN("~Buffer()");

    assert(!myBufferDesc.initialData);

    // todo: put on command context delete queue?
    vmaDestroyBuffer(myBufferDesc.deviceContext->getAllocator(), myBuffer, myBufferMemory);

    --ourDebugCount;
}
