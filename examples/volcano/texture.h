#pragma once

#include "gfx-types.h"
#include "utils.h"

#include <filesystem>

template <GraphicsBackend B>
struct TextureCreateDesc
{
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t channelCount = 0;
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
class Texture : Noncopyable
{
public:

    Texture(TextureCreateDesc<B>&& desc,
        DeviceHandle<B> device, CommandPoolHandle<B> commandPool, QueueHandle<B> queue, AllocatorHandle<B> allocator);

    Texture(const std::filesystem::path& textureFile,
        DeviceHandle<B> device, CommandPoolHandle<B> commandPool, QueueHandle<B> queue, AllocatorHandle<B> allocator);

    ~Texture();

    const auto& getDesc() const { return myDesc; }
    
    const auto getImage() const { return myImage; }
    const auto getImageMemory() const { return myImageMemory; }

    ImageViewHandle<B> createView(Flags<B> aspectFlags) const;
    
private:

    const TextureCreateDesc<B> myDesc = {};

    DeviceHandle<B> myDevice = 0;
    AllocatorHandle<B> myAllocator = 0;

	ImageHandle<B> myImage = 0;
	AllocationHandle<B> myImageMemory = 0;
};
