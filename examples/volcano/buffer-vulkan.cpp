#include "buffer.h"
#include "vk-utils.h"

#include <stb_sprintf.h>

template <>
Buffer<Vk>::Buffer(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    std::tuple<
        BufferCreateDesc<Vk>,
        BufferHandle<Vk>,
        AllocationHandle<Vk>>&& descAndData)
: DeviceResource<Vk>(
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
Buffer<Vk>::Buffer(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    BufferCreateDesc<Vk>&& desc)
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
Buffer<Vk>::Buffer(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    const std::shared_ptr<CommandContext<Vk>>& commandContext,
    std::tuple<
        BufferCreateDesc<Vk>,
        BufferHandle<Vk>,
        AllocationHandle<Vk>>&& descAndInitialData)
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
Buffer<Vk>::~Buffer()
{
    if (auto buffer = getBufferHandle(); buffer)
        getDeviceContext()->addTimelineCallback(
            [allocator = getDeviceContext()->getAllocator(), buffer, bufferMemory = getBufferMemory()](uint64_t){
                vmaDestroyBuffer(allocator, buffer, bufferMemory);
        });
}

template <>
BufferView<Vk>::BufferView(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    BufferViewHandle<Vk>&& bufferView)
: DeviceResource<Vk>(
    deviceContext,
    {"_View"},
    1,
    VK_OBJECT_TYPE_BUFFER_VIEW,
    reinterpret_cast<uint64_t*>(&bufferView))
, myBufferView(std::move(bufferView))
{
}

template <>
BufferView<Vk>::BufferView(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    const Buffer<Vk>& buffer,
    Format<Vk> format,
    DeviceSize<Vk> offset,
    DeviceSize<Vk> range)
: BufferView<Vk>(
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
BufferView<Vk>::~BufferView()
{
    if (auto bufferView = getBufferViewHandle(); bufferView)
        getDeviceContext()->addTimelineCallback(
            [device = getDeviceContext()->getDevice(), bufferView](uint64_t){
                vkDestroyBufferView(device, bufferView, nullptr);
        });
}
