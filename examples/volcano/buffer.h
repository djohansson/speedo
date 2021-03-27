#pragma once

#include "command.h"
#include "device.h"
#include "types.h"

#include <memory>
#include <tuple>

template <GraphicsBackend B>
struct BufferCreateDesc
{
    DeviceSize<B> size = {};
    Flags<B> usageFlags = {};
    Flags<B> memoryFlags = {};
};

template <GraphicsBackend B>
class Buffer : public DeviceObject<B>
{
    using ValueType = std::tuple<BufferHandle<B>, AllocationHandle<B>>;

public:

    constexpr Buffer() noexcept = default;
    Buffer(Buffer&& other) noexcept;
    Buffer( // creates uninitialized buffer
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        BufferCreateDesc<B>&& desc);
    Buffer( // copies initialData into the target, using a temporary internal staging buffer if needed.
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        CommandPoolContext<B>& commandContext,
        BufferCreateDesc<B>&& desc,
        const void* initialData);
    ~Buffer();

    Buffer& operator=(Buffer&& other) noexcept;
    operator auto() const { return std::get<0>(myBuffer); }

    void swap(Buffer& rhs) noexcept;
    friend void swap(Buffer& lhs, Buffer& rhs) noexcept { lhs.swap(rhs); }

    const auto& getDesc() const { return myDesc; }
    const auto& getBufferMemory() const { return std::get<1>(myBuffer); }

protected:

    Buffer( // copies buffer in descAndInitialData into the target. descAndInitialData buffer gets automatically garbage collected when copy has finished.
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        CommandPoolContext<B>& commandContext,
        std::tuple<BufferCreateDesc<B>, BufferHandle<B>, AllocationHandle<B>>&& descAndInitialData);

private:

    Buffer( // takes ownership of provided buffer handle and allocation
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        BufferCreateDesc<B>&& desc,
        ValueType&& buffer);

    BufferCreateDesc<B> myDesc = {};
    ValueType myBuffer = {};
};

template <GraphicsBackend B>
class BufferView : public DeviceObject<B>
{
public:
    
    constexpr BufferView() noexcept = default;
    BufferView(BufferView&& other) noexcept;
    BufferView( // creates a view from buffer
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        const Buffer<B>& buffer,
        Format<B> format,
        DeviceSize<B> offset,
        DeviceSize<B> range);
    ~BufferView();

    BufferView& operator=(BufferView&& other) noexcept;
    operator auto() const { return myView; }

    void swap(BufferView& rhs) noexcept;
    friend void swap(BufferView& lhs, BufferView& rhs) noexcept { lhs.swap(rhs); }

private:

    BufferView( // uses provided image view
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        BufferViewHandle<B>&& view);

    BufferViewHandle<B> myView = {};
};
