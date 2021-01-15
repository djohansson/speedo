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
    using ValueType = std::tuple<BufferHandle<B>, AllocationHandle<B>>;

public:

    Buffer(Buffer&& other);
    Buffer( // creates uninitialized buffer
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        BufferCreateDesc<B>&& desc);
    Buffer( // copies initialData into the target, using a temporary internal staging buffer if needed.
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        const std::shared_ptr<CommandContext<B>>& commandContext,
        BufferCreateDesc<B>&& desc,
        const void* initialData,
        size_t initialDataSize);
    ~Buffer();

    Buffer& operator=(Buffer&& other);
    operator auto() const { return std::get<0>(myBuffer); }

    const auto& getDesc() const { return myDesc; }
    const auto& getBufferMemory() const { return std::get<1>(myBuffer); }

protected:

    Buffer( // copies buffer in descAndInitialData into the target. descAndInitialData buffer gets automatically garbage collected when copy has finished.
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        const std::shared_ptr<CommandContext<B>>& commandContext,
        std::tuple<BufferCreateDesc<B>, BufferHandle<B>, AllocationHandle<B>>&& descAndInitialData);

private:

    Buffer( // takes ownership of provided buffer handle and allocation
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        BufferCreateDesc<B>&& desc,
        ValueType&& buffer);

    const BufferCreateDesc<B> myDesc = {};
    ValueType myBuffer = {};
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
    operator auto() const { return myView; }

private:

    BufferView( // uses provided image view
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        BufferViewHandle<B>&& bufferView);

    BufferViewHandle<B> myView = {};
};
