#pragma once

#include "command.h"
#include "device.h"
#include "deviceresource.h"
#include "gfx-types.h"

#include <memory>

template <GraphicsBackend B>
struct BufferCreateDesc : DeviceResourceCreateDesc<B>
{
    DeviceSize<B> size = 0;
    Flags<B> usageFlags = 0;
    Flags<B> memoryFlags = 0;
};

template <GraphicsBackend B>
class Buffer : public DeviceResource<B>
{
public:

    Buffer(Buffer&& other) noexcept = default;
    Buffer( // creates uninitialized buffer
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        BufferCreateDesc<B>&& desc);
    Buffer( // copies the initial buffer into a new one. buffer gets garbage collected when finished copying.
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        const std::shared_ptr<CommandContext<B>>& commandContext,
        std::tuple<BufferCreateDesc<B>, BufferHandle<B>, AllocationHandle<B>>&& descAndInitialData);
    ~Buffer();

    const auto& getDesc() const { return myDesc; }
    const auto& getBufferHandle() const { return std::get<0>(myData); }
    const auto& getBufferMemory() const { return std::get<1>(myData); }

private:

    Buffer( // uses provided buffer
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        std::tuple<BufferCreateDesc<B>, BufferHandle<B>, AllocationHandle<B>>&& descAndData);

    const BufferCreateDesc<B> myDesc = {};
    std::tuple<BufferHandle<B>, AllocationHandle<B>> myData = {};
};

template <GraphicsBackend B>
class BufferView : public DeviceResource<B>
{
public:
    
    BufferView(BufferView&& other) noexcept = default;
    BufferView( // creates a view from buffer
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        const Buffer<B>& buffer,
        Format<B> format,
        DeviceSize<B> offset,
        DeviceSize<B> range);
    ~BufferView();

    auto getBufferViewHandle() const { return myBufferView; }

private:

    BufferView( // uses provided image view
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        BufferViewHandle<B>&& bufferView);

    BufferViewHandle<B> myBufferView = 0;
};
