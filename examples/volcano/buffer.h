#pragma once

#include "gfx-types.h"

#include <memory>

template <GraphicsBackend B>
struct BufferCreateDesc
{
    DeviceSize<B> size = 0;
    Flags<B> usageFlags = 0;
    Flags<B> memoryFlags = 0;
    // todo: avoid temp copy - copy directly from mapped memory to gpu
    std::unique_ptr<std::byte[]> initialData;
    std::string debugName;
};

template <GraphicsBackend B>
class Buffer
{
public:

    Buffer(
        DeviceHandle<B> device, AllocatorHandle<B> allocator,
        BufferCreateDesc<B>&& desc);

    ~Buffer();

    const auto& getDesc() const { return myDesc; }
    
    const auto getBuffer() const { return myBuffer; }
    const auto getBufferMemory() const { return myBufferMemory; }
    
    BufferViewHandle<B> createView(Format<B> format, DeviceSize<B> offset, DeviceSize<B> range) const;

private:

    DeviceHandle<B> myDevice = 0; 
    AllocatorHandle<B> myAllocator = 0;

    BufferCreateDesc<B> myDesc = {};

	BufferHandle<B> myBuffer = 0;
	AllocationHandle<B> myBufferMemory = 0;
};
