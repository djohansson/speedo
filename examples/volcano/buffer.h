#pragma once

#include "gfx-types.h"

#include <memory>

template <GraphicsBackend B>
struct BufferData
{
    DeviceSize<B> bufferSize = 0;
    std::unique_ptr<std::byte[]> bufferData;
    Format<B> format;
    Flags<B> usageFlags = 0;
    Flags<B> memoryFlags = 0;
    DeviceSize<B> offset = 0;
    DeviceSize<B> range = 0;
    std::string debugName;
};

template <GraphicsBackend B>
struct Buffer
{
    Buffer(
        DeviceHandle<B> device, AllocatorHandle<B> allocator,
        const BufferData<B>& data);

    ~Buffer();

    DeviceHandle<B> device = 0; 
    AllocatorHandle<B> allocator = 0;

	BufferHandle<B> buffer = 0;
	AllocationHandle<B> bufferMemory = 0;
    BufferViewHandle<B> bufferView = 0;

    Format<B> format;
};
