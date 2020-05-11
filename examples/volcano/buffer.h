#pragma once

#include "command.h"
#include "device.h"
#include "gfx-types.h"
#include "resource.h"

#include <memory>

template <GraphicsBackend B>
struct BufferCreateDesc : ResourceCreateDesc<B>
{
    DeviceSize<B> size = 0;
    Flags<B> usageFlags = 0;
    Flags<B> memoryFlags = 0;
};

template <GraphicsBackend B>
class Buffer : Resource<B>
{
public:

    Buffer(Buffer&& other) = default;
    Buffer( // creates uninitialized buffer
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        BufferCreateDesc<B>&& desc);
    Buffer( // uses provided buffer
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        std::tuple<BufferCreateDesc<B>, BufferHandle<B>, AllocationHandle<B>>&& descAndData);
    Buffer( // copies the initial buffer into a new one. buffer gets garbage collected when finished copying.
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        const std::shared_ptr<CommandContext<B>>& commandContext,
        std::tuple<BufferCreateDesc<B>, BufferHandle<B>, AllocationHandle<B>>&& descAndInitialData);
    ~Buffer();

    const auto& getDesc() const { return myDesc; }
    const auto& getBuffer() const { return std::get<0>(myData); }
    const auto& getBufferMemory() const { return std::get<1>(myData); }
    
    BufferViewHandle<B> createView(Format<B> format, DeviceSize<B> offset, DeviceSize<B> range) const;

private:

    const BufferCreateDesc<B> myDesc = {};
    std::tuple<BufferHandle<B>, AllocationHandle<B>> myData = {};
};
