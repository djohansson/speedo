#pragma once

#include "gfx-types.h"
#include "utils.h"

#include <filesystem>

template <GraphicsBackend B>
struct TextureCreateDesc
{
    DeviceHandle<B> device = 0;
    AllocatorHandle<B> allocator = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t channelCount = 0;
    DeviceSize<B> size = 0;
    Format<B> format;
    Flags<B> usage = 0;
    // these will be destroyed when calling deleteInitialData()
    BufferHandle<B> initialData = 0;
    AllocationHandle<B> initialDataMemory = 0;
    //
    std::string debugName;
};

template <GraphicsBackend B>
class Texture : Noncopyable
{
public:

    Texture(TextureCreateDesc<B>&& desc, CommandBufferHandle<B> commandBuffer);
    Texture(const std::filesystem::path& textureFile,
        DeviceHandle<B> device, AllocatorHandle<B> allocator, CommandBufferHandle<B> commandBuffer);

    ~Texture();

    void deleteInitialData();

    const auto& getDesc() const { return myDesc; }
    const auto getImage() const { return myImage; }
    const auto getImageMemory() const { return myImageMemory; }

    ImageViewHandle<B> createView(Flags<B> aspectFlags) const;
    
private:

    void waitForTransferComplete();

    TextureCreateDesc<B> myDesc = {};
	ImageHandle<B> myImage = 0;
	AllocationHandle<B> myImageMemory = 0;
};
