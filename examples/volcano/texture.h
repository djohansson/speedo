#pragma once

#include "command.h"
#include "device.h"
#include "gfx-types.h"
#include "utils.h"

#include <filesystem>

template <GraphicsBackend B>
struct TextureDesc
{
    std::shared_ptr<DeviceContext<B>> deviceContext;
    Extent2d<B> extent = {};
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

    Texture(TextureDesc<B>&& desc, CommandContext<B>& commandContext);
    Texture(const std::filesystem::path& textureFile, CommandContext<B>& commandContext);
    ~Texture();

    static uint32_t ourDebugCount;

    const auto& getTextureDesc() const { return myTextureDesc; }
    const auto& getImage() const { return myImage; }
    const auto& getImageMemory() const { return myImageMemory; }

    ImageViewHandle<B> createView(Flags<B> aspectFlags) const;
    
private:

    void deleteInitialData();

    // todo: mipmaps
    TextureDesc<B> myTextureDesc = {};
	ImageHandle<B> myImage = 0;
	AllocationHandle<B> myImageMemory = 0;
    //
};
