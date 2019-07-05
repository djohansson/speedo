#pragma once

#include "gfx-types.h"

#include <filesystem>
#include <memory>

template <GraphicsBackend B>
struct TextureData
{
    uint32_t nx = 0;
    uint32_t ny = 0;
    uint32_t nChannels = 0;
    DeviceSize<B> imageSize = 0;
    std::unique_ptr<std::byte[]> imageData;
    Format<B> format;
    Flags<B> flags = 0;
    Flags<B> aspectFlags;
    std::string debugName;
};

template <GraphicsBackend B>
struct Texture
{
    Texture(
        DeviceHandle<B> device, CommandPoolHandle<B> commandPool, QueueHandle<B> queue, AllocatorHandle<B> allocator,
        const TextureData<B>& data);

    Texture(
        DeviceHandle<B> device, CommandPoolHandle<B> commandPool, QueueHandle<B> queue, AllocatorHandle<B> allocator,
        const std::filesystem::path& textureFile);

    ~Texture();

    DeviceHandle<B> device = 0; 
    AllocatorHandle<B> allocator = 0;

	ImageHandle<B> image = 0;
	AllocationHandle<B> imageMemory = 0;
	ImageViewHandle<B> imageView = 0;

    Format<B> format;
};
