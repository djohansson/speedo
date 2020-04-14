#pragma once

#include "command.h"
#include "gfx-types.h"
#include "utils.h"

#include <memory>

template <GraphicsBackend B>
struct BufferDesc
{
    std::shared_ptr<DeviceContext<B>> deviceContext;
    DeviceSize<B> size = 0;
    Flags<B> usageFlags = 0;
    Flags<B> memoryFlags = 0;
    // these will be destroyed when calling deleteInitialData()
    BufferHandle<B> initialData = 0;
    AllocationHandle<B> initialDataMemory = 0;
    //
    // todo: reconsider.
    std::string debugName;
    //
};

template <GraphicsBackend B>
class Model;

template <GraphicsBackend B>
class Buffer : Noncopyable
{
    friend class Model<B>;

public:

    Buffer(BufferDesc<B>&& desc, CommandContext<B>& commands);
    ~Buffer();

    const auto& getBufferDesc() const { return myDesc; }
    const auto& getBuffer() const { return myBuffer; }
    const auto& getBufferMemory() const { return myBufferMemory; }
    
    BufferViewHandle<B> createView(Format<B> format, DeviceSize<B> offset, DeviceSize<B> range) const;

private:

    void deleteInitialData();

    BufferDesc<B> myDesc = {};
	BufferHandle<B> myBuffer = 0;
	AllocationHandle<B> myBufferMemory = 0;
};
