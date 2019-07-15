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
    Flags<B> usage = 0;
    // these will be destroyed in Texture:s constructor
    BufferHandle<B> initialData = 0;
    AllocationHandle<B> initialDataMemory = 0;
    //
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

    ImageViewHandle<B> createView(Flags<B> aspectFlags) const;
    
private:

    DeviceHandle<B> myDevice = 0;
    AllocatorHandle<B> myAllocator = 0;

    TextureCreateDesc<B> myDesc = {};

	ImageHandle<B> myImage = 0;
	AllocationHandle<B> myImageMemory = 0;
};
