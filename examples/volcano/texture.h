#pragma once

#include "gfx-types.h"

template <GraphicsBackend B>
struct Texture
{
    Texture(
        DeviceHandle<B> device, CommandPoolHandle<B> commandPool, QueueHandle<B> queue, AllocatorHandle<B> allocator,
        const std::byte* data, int x, int y, Format<B> format,
        ImageUsageFlags<B> flags, ImageAspectFlagBits<B> aspectFlags,
        const char* debugName = nullptr);

    ~Texture();

    DeviceHandle<B> device = 0; 
    AllocatorHandle<B> allocator = 0;

	ImageHandle<B> image = 0;
	AllocationHandle<B> imageMemory = 0;
	ImageViewHandle<B> imageView = 0;

    Format<B> format;
};
