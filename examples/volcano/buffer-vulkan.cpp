#include "buffer.h"
#include "vk-utils.h"

#include <string>


template <>
BufferViewHandle<GraphicsBackend::Vulkan>
Buffer<GraphicsBackend::Vulkan>::createView(
    Format<GraphicsBackend::Vulkan> format,
    DeviceSize<GraphicsBackend::Vulkan> offset,
    DeviceSize<GraphicsBackend::Vulkan> range)
{
    auto view = createBufferView(getDeviceContext()->getDevice(), getBuffer(), 0, format, offset, range);
    
    static std::atomic_uint32_t viewIndex = 0;
    char stringBuffer[256];
    static constexpr std::string_view bufferViewStr = "_BufferView";
    sprintf_s(
        stringBuffer,
        sizeof(stringBuffer),
        "%.*s%.*s%u",
        getName().size(),
        getName().c_str(),
        static_cast<int>(bufferViewStr.size()),
        bufferViewStr.data(),
        viewIndex++);
    addObject(VK_OBJECT_TYPE_BUFFER_VIEW, reinterpret_cast<uint64_t>(view), stringBuffer);
    
    return view;
}

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
    deviceContext->addGarbageCollectCallback([deviceContext, descAndInitialData](uint64_t){
        vmaDestroyBuffer(deviceContext->getAllocator(), std::get<1>(descAndInitialData), std::get<2>(descAndInitialData));
    });
}

template <>
Buffer<GraphicsBackend::Vulkan>::~Buffer()
{
    getDeviceContext()->addGarbageCollectCallback(
        [allocator = getDeviceContext()->getAllocator(), buffer = getBuffer(), bufferMemory = getBufferMemory()](uint64_t){
            vmaDestroyBuffer(allocator, buffer, bufferMemory);
    });
}
