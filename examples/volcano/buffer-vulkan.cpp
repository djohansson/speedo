#include "buffer.h"
#include "vk-utils.h"

#include <string>

#include <core/slang-secure-crt.h>


template <>
Buffer<GraphicsBackend::Vulkan>::Buffer(
    const std::shared_ptr<DeviceContext<GraphicsBackend::Vulkan>>& deviceContext,
    std::tuple<
        BufferCreateDesc<GraphicsBackend::Vulkan>,
        BufferHandle<GraphicsBackend::Vulkan>,
        AllocationHandle<GraphicsBackend::Vulkan>>&& descAndData)
: DeviceResource<GraphicsBackend::Vulkan>(
    deviceContext,
    std::get<0>(descAndData),
    1,
    VK_OBJECT_TYPE_BUFFER,
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
            desc.name.c_str())))
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
            std::get<0>(descAndInitialData).name.c_str())))
{
    commandContext->addSubmitFinishedCallback([deviceContext, descAndInitialData](uint64_t){
        vmaDestroyBuffer(deviceContext->getAllocator(), std::get<1>(descAndInitialData), std::get<2>(descAndInitialData));
    });
}

template <>
Buffer<GraphicsBackend::Vulkan>::~Buffer()
{
    if (auto buffer = getBufferHandle(); buffer)
        getDeviceContext()->addTimelineCallback(
            [allocator = getDeviceContext()->getAllocator(), buffer, bufferMemory = getBufferMemory()](uint64_t){
                vmaDestroyBuffer(allocator, buffer, bufferMemory);
        });
}

template <>
BufferView<GraphicsBackend::Vulkan>::BufferView(
    const std::shared_ptr<DeviceContext<GraphicsBackend::Vulkan>>& deviceContext,
    BufferViewHandle<GraphicsBackend::Vulkan>&& bufferView)
: DeviceResource<GraphicsBackend::Vulkan>(
    deviceContext,
    {"_View"},
    1,
    VK_OBJECT_TYPE_BUFFER_VIEW,
    reinterpret_cast<uint64_t*>(&bufferView))
, myBufferView(std::move(bufferView))
{
}

template <>
BufferView<GraphicsBackend::Vulkan>::BufferView(
    const std::shared_ptr<DeviceContext<GraphicsBackend::Vulkan>>& deviceContext,
    const Buffer<GraphicsBackend::Vulkan>& buffer,
    Format<GraphicsBackend::Vulkan> format,
    DeviceSize<GraphicsBackend::Vulkan> offset,
    DeviceSize<GraphicsBackend::Vulkan> range)
: BufferView<GraphicsBackend::Vulkan>(
    deviceContext,
    createBufferView(
        deviceContext->getDevice(),
        buffer.getBufferHandle(),
        0, // "reserved for future use"
        format,
        offset,
        range))
{
}

template <>
BufferView<GraphicsBackend::Vulkan>::~BufferView()
{
    if (auto bufferView = getBufferViewHandle(); bufferView)
        getDeviceContext()->addTimelineCallback(
            [device = getDeviceContext()->getDevice(), bufferView](uint64_t){
                vkDestroyBufferView(device, bufferView, nullptr);
        });
}
