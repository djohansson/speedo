#pragma once

#include "command.h"
#include "device.h"
#include "types.h"

#include <memory>
#include <tuple>

template <GraphicsBackend B>
struct BufferCreateDesc : DeviceResourceCreateDesc<B>
{
    DeviceSize<B> size = {};
    Flags<B> usageFlags = {};
    Flags<B> memoryFlags = {};
};

template <GraphicsBackend B>
class Buffer : public DeviceResource<B>
{
public:

    Buffer(Buffer&& other);
    Buffer( // creates uninitialized buffer
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        BufferCreateDesc<B>&& desc);
    Buffer( // copies the initial buffer into a new one. buffer gets garbage collected when finished copying.
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        const std::shared_ptr<CommandContext<B>>& commandContext,
        std::tuple<BufferCreateDesc<B>, BufferHandle<B>, AllocationHandle<B>>&& descAndInitialData);
    Buffer( // takes ownership of provided buffer handle and allocation
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        std::tuple<BufferCreateDesc<B>, BufferHandle<B>, AllocationHandle<B>>&& descAndData);
    ~Buffer();

    Buffer& operator=(Buffer&& other);
    operator auto() const { return std::get<0>(myData); }

    const auto& getDesc() const { return myDesc; }
    const auto& getBufferMemory() const { return std::get<1>(myData); }

private:

    const BufferCreateDesc<B> myDesc = {};
    std::tuple<BufferHandle<B>, AllocationHandle<B>> myData = {};
};

template <GraphicsBackend B>
class BufferView : public DeviceResource<B>
{
public:
    
    BufferView(BufferView&& other);
    BufferView( // creates a view from buffer
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        const Buffer<B>& buffer,
        Format<B> format,
        DeviceSize<B> offset,
        DeviceSize<B> range);
    ~BufferView();

    BufferView& operator=(BufferView&& other);
    operator auto() const { return myBufferView; }

private:

    BufferView( // uses provided image view
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        BufferViewHandle<B>&& bufferView);

    BufferViewHandle<B> myBufferView = {};
};
