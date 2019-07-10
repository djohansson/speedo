#pragma once

#include "gfx-types.h"

#include <filesystem>
#include <memory>

template <GraphicsBackend B>
struct TextureCreateDesc
{
    uint32_t nx = 0;
    uint32_t ny = 0;
    uint32_t nChannels = 0;
    DeviceSize<B> size = 0;
    Format<B> format;
    Flags<B> flags = 0;
    Flags<B> aspectFlags;
    // todo: avoid temp copy - copy directly from mapped memory to gpu
    std::unique_ptr<std::byte[]> initialData;
    std::string debugName;
};

template <GraphicsBackend B>
class Texture
{
public:

    Texture(
        DeviceHandle<B> device, CommandPoolHandle<B> commandPool, QueueHandle<B> queue, AllocatorHandle<B> allocator,
        TextureCreateDesc<B>&& data);

    Texture(
        DeviceHandle<B> device, CommandPoolHandle<B> commandPool, QueueHandle<B> queue, AllocatorHandle<B> allocator,
        const std::filesystem::path& textureFile);

    ~Texture();

    const auto& getDesc() const { return myDesc; }
    
    const auto getImage() const { return myImage; }
    const auto getImageMemory() const { return myImageMemory; }
    const auto getImageView() const { return myImageView; }

private:

    TextureCreateDesc<B> myDesc;

    DeviceHandle<B> myDevice = 0; 
    AllocatorHandle<B> myAllocator = 0;

	ImageHandle<B> myImage = 0;
	AllocationHandle<B> myImageMemory = 0;
	ImageViewHandle<B> myImageView = 0;
};
