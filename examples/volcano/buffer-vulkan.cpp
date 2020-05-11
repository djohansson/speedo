#include "buffer.h"
#include "vk-utils.h"

#include <string>


template <>
BufferViewHandle<GraphicsBackend::Vulkan>
Buffer<GraphicsBackend::Vulkan>::createView(
    Format<GraphicsBackend::Vulkan> format,
    DeviceSize<GraphicsBackend::Vulkan> offset,
    DeviceSize<GraphicsBackend::Vulkan> range) const
{
    return createBufferView(getDeviceContext()->getDevice(), getBuffer(), 0, format, offset, range);
}

template <>
Buffer<GraphicsBackend::Vulkan>::Buffer(
    const std::shared_ptr<DeviceContext<GraphicsBackend::Vulkan>>& deviceContext,
    std::tuple<
        BufferCreateDesc<GraphicsBackend::Vulkan>,
        BufferHandle<GraphicsBackend::Vulkan>,
        AllocationHandle<GraphicsBackend::Vulkan>>&& descAndData)
: Resource<GraphicsBackend::Vulkan>(
    deviceContext,
    std::get<0>(descAndData),
    VK_OBJECT_TYPE_BUFFER,
    1,
    reinterpret_cast<uint64_t*>(&std::get<1>(descAndData)))
, myDesc(std::move(std::get<0>(descAndData)))
, myData(std::make_tuple(std::get<1>(descAndData), std::get<2>(descAndData)))
{
}

template <>
Buffer<GraphicsBackend::Vulkan>::Buffer(
    const std::shared_ptr<DeviceContext<GraphicsBackend::Vulkan>>& deviceContext,
    BufferCreateDesc<GraphicsBackend::Vulkan>&& desc)
: Buffer(
    deviceContext,
    std::tuple_cat(
        std::make_tuple(std::move(desc)),
        createBuffer(
            deviceContext->getAllocator(),
            desc.size,
            desc.usageFlags,
            desc.memoryFlags,
            desc.name)))
{
}

template <>
Buffer<GraphicsBackend::Vulkan>::Buffer(
    const std::shared_ptr<DeviceContext<GraphicsBackend::Vulkan>>& deviceContext,
    const std::shared_ptr<CommandContext<GraphicsBackend::Vulkan>>& commandContext,
    std::tuple<
        BufferCreateDesc<GraphicsBackend::Vulkan>,
        BufferHandle<GraphicsBackend::Vulkan>,
        AllocationHandle<GraphicsBackend::Vulkan>>&& descAndInitialData)
: Buffer(
    deviceContext,
    std::tuple_cat(
        std::make_tuple(std::move(std::get<0>(descAndInitialData))),
        createBuffer(
     		commandContext->commands(),
            deviceContext->getAllocator(),
            std::get<1>(descAndInitialData),
            std::get<0>(descAndInitialData).size,
            std::get<0>(descAndInitialData).usageFlags,
            std::get<0>(descAndInitialData).memoryFlags,
            std::get<0>(descAndInitialData).name)))
{
    deviceContext->addResourceGarbageCollectCallback([deviceContext, descAndInitialData](uint64_t){
        vmaDestroyBuffer(deviceContext->getAllocator(), std::get<1>(descAndInitialData), std::get<2>(descAndInitialData));
    }, deviceContext->timelineValue()->fetch_add(1, std::memory_order_relaxed));
}

template <>
Buffer<GraphicsBackend::Vulkan>::~Buffer()
{
    // todo: put on command context delete queue?
    vmaDestroyBuffer(getDeviceContext()->getAllocator(), getBuffer(), getBufferMemory());
}
