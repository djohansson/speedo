#pragma once

#include "gfx-types.h"

#include <memory>

template <GraphicsBackend B>
struct BufferCreateDesc
{
    DeviceSize<B> size = 0;
    Format<B> format;
    Flags<B> usageFlags = 0;
    Flags<B> memoryFlags = 0;
    DeviceSize<B> offset = 0;
    DeviceSize<B> range = 0;
    const std::byte* initialData = nullptr;
    const char* debugName = nullptr;
};

template <GraphicsBackend B>
struct Buffer
{
    Buffer(
        DeviceHandle<B> device, AllocatorHandle<B> allocator,
        BufferCreateDesc<B>&& desc);

    ~Buffer();

    DeviceHandle<B> device = 0; 
    AllocatorHandle<B> allocator = 0;

    BufferCreateDesc<B> desc;

	BufferHandle<B> buffer = 0;
	AllocationHandle<B> bufferMemory = 0;
    BufferViewHandle<B> bufferView = 0;
};
