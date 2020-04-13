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

    Texture(TextureDesc<B>&& desc, const CommandContext<B>& commands);
    Texture(const std::filesystem::path& textureFile, const CommandContext<B>& commands);

    ~Texture();

    void deleteInitialData(); // todo: make priavte and automagic

    const auto& getTextureDesc() const { return myDesc; }
    const auto& getImage() const { return myImage; }
    const auto& getImageMemory() const { return myImageMemory; }

    ImageViewHandle<B> createView(Flags<B> aspectFlags) const;
    
private:

    // todo: mipmaps
    TextureDesc<B> myDesc = {};
	ImageHandle<B> myImage = 0;
	AllocationHandle<B> myImageMemory = 0;
    //
};
