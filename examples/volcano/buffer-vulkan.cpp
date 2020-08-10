#include "buffer.h"
#include "vk-utils.h"

#include <core/slang-secure-crt.h>

template <>
Buffer<Vk>::Buffer(
    const std::shared_ptr<ApplicationContext<Vk>>& appContext,
    BufferCreateDesc<Vk>&& desc,
    std::tuple<BufferHandle<Vk>, AllocationHandle<Vk>>&& data)
: DeviceResource<Vk>(
    appContext->device,
    desc,
    1,
    VK_OBJECT_TYPE_BUFFER,
    reinterpret_cast<uint64_t*>(&std::get<0>(data)))
, myDesc(std::move(desc))
, myData(std::move(data))
{
}

template <>
Buffer<Vk>::Buffer(
    const std::shared_ptr<ApplicationContext<Vk>>& appContext,
    BufferCreateDesc<Vk>&& desc)
: Buffer(
    appContext,
    std::move(desc),
    createBuffer(
        appContext->device->getAllocator(),
        desc.size,
        desc.usageFlags,
        desc.memoryFlags,
        desc.name.c_str()))
{
}

template <>
Buffer<Vk>::Buffer(
    const std::shared_ptr<ApplicationContext<Vk>>& appContext,
    std::tuple<
        BufferCreateDesc<Vk>,
        BufferHandle<Vk>,
        AllocationHandle<Vk>>&& descAndInitialData)
: Buffer(
    appContext,
    std::move(std::get<0>(descAndInitialData)),
    createBuffer(
        appContext->transferCommands->commands(),
        appContext->device->getAllocator(),
        std::get<1>(descAndInitialData),
        std::get<0>(descAndInitialData).size,
        std::get<0>(descAndInitialData).usageFlags,
        std::get<0>(descAndInitialData).memoryFlags,
        std::get<0>(descAndInitialData).name.c_str()))
{
    appContext->transferCommands->addSubmitFinishedCallback([appContext, descAndInitialData](uint64_t){
        vmaDestroyBuffer(appContext->device->getAllocator(), std::get<1>(descAndInitialData), std::get<2>(descAndInitialData));
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
    const std::shared_ptr<ApplicationContext<Vk>>& appContext,
    BufferViewHandle<Vk>&& bufferView)
: DeviceResource<Vk>(
    appContext->device,
    {"_View"},
    1,
    VK_OBJECT_TYPE_BUFFER_VIEW,
    reinterpret_cast<uint64_t*>(&bufferView))
, myBufferView(std::move(bufferView))
{
}

template <>
BufferView<Vk>::BufferView(
    const std::shared_ptr<ApplicationContext<Vk>>& appContext,
    const Buffer<Vk>& buffer,
    Format<Vk> format,
    DeviceSize<Vk> offset,
    DeviceSize<Vk> range)
: BufferView<Vk>(
    appContext,
    createBufferView(
        appContext->device->getDevice(),
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
