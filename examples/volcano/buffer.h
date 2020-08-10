#pragma once

#include "applicationcontext.h"
#include "types.h"

#include <memory>
#include <tuple>

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

    Buffer(Buffer&& other) = default;
    Buffer( // creates uninitialized buffer
        const std::shared_ptr<ApplicationContext<B>>& appContext,
        BufferCreateDesc<B>&& desc);
    Buffer( // copies the initial buffer into a new one. buffer gets garbage collected when finished copying.
        const std::shared_ptr<ApplicationContext<B>>& appContext,
        std::tuple<BufferCreateDesc<B>, BufferHandle<B>, AllocationHandle<B>>&& descAndInitialData);
    ~Buffer();

    Buffer& operator=(Buffer&& other) = default;

    const auto& getDesc() const { return myDesc; }
    const auto& getBufferHandle() const { return std::get<0>(myData); }
    const auto& getBufferMemory() const { return std::get<1>(myData); }

private:

    Buffer( // uses provided buffer
        const std::shared_ptr<ApplicationContext<B>>& appContext,
        BufferCreateDesc<B>&& desc,
        std::tuple<BufferHandle<B>, AllocationHandle<B>>&& data);

    const BufferCreateDesc<B> myDesc = {};
    std::tuple<BufferHandle<B>, AllocationHandle<B>> myData = {};
};

template <GraphicsBackend B>
class BufferView : public DeviceResource<B>
{
public:
    
    BufferView(BufferView&& other) = default;
    BufferView( // creates a view from buffer
        const std::shared_ptr<ApplicationContext<B>>& appContext,
        const Buffer<B>& buffer,
        Format<B> format,
        DeviceSize<B> offset,
        DeviceSize<B> range);
    ~BufferView();

    BufferView& operator=(BufferView&& other) = default;

    auto getBufferViewHandle() const { return myBufferView; }

private:

    BufferView( // uses provided image view
        const std::shared_ptr<ApplicationContext<B>>& appContext,
        BufferViewHandle<B>&& bufferView);

    BufferViewHandle<B> myBufferView = 0;
};
