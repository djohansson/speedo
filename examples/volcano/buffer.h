#pragma once

#include "gfx-types.h"
#include "utils.h"

#include <memory>

template <GraphicsBackend B>
struct BufferCreateDesc
{
    DeviceSize<B> size = 0;
    Flags<B> usageFlags = 0;
    Flags<B> memoryFlags = 0;
    // these will be destroyed in Buffer:s constructor
    BufferHandle<B> initialData = 0;
    AllocationHandle<B> initialDataMemory = 0;
    //
    std::string debugName;
};

template <GraphicsBackend B>
class Buffer : Noncopyable
{
public:

    Buffer(BufferCreateDesc<B>&& desc,
        DeviceHandle<B> device, VkCommandPool commandPool, VkQueue queue, AllocatorHandle<B> allocator); // todo: bake into create desc
    ~Buffer();

    const auto& getDesc() const { return myDesc; }
    
    const auto getBuffer() const { return myBuffer; }
    const auto getBufferMemory() const { return myBufferMemory; }
    
    BufferViewHandle<B> createView(Format<B> format, DeviceSize<B> offset, DeviceSize<B> range) const;

private:

    const BufferCreateDesc<B> myDesc = {};

    DeviceHandle<B> myDevice = 0; 
    AllocatorHandle<B> myAllocator = 0;

	BufferHandle<B> myBuffer = 0;
	AllocationHandle<B> myBufferMemory = 0;
};
