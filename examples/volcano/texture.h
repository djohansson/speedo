#pragma once

#include "command.h"
#include "device.h"
#include "gfx-types.h"

#include <filesystem>

template <GraphicsBackend B>
struct TextureCreateDesc
{
    std::shared_ptr<DeviceContext<B>> deviceContext;
    Extent2d<B> extent = {};
    uint32_t channelCount = 0;
    Format<B> format = {};
    Flags<B> usage = 0;
    // these will be destroyed when calling deleteInitialData()
    BufferHandle<B> initialData = 0;
    AllocationHandle<B> initialDataMemory = 0;
    //
    std::string debugName;
};

template <GraphicsBackend B>
class Texture
{
public:

    Texture(Texture&& other) = default;
    Texture(TextureCreateDesc<B>&& desc, CommandContext<B>& commandContext);
    Texture(const std::filesystem::path& textureFile, CommandContext<B>& commandContext);
    ~Texture();

    static uint32_t ourDebugCount;

    const auto& getDesc() const { return myDesc; }
    const auto& getImage() const { return myImage; }
    const auto& getImageMemory() const { return myImageMemory; }
    const auto& getImageLayout() const { return myImageLayout; }

    ImageViewHandle<B> createView(Flags<B> aspectFlags) const;

    void transition(CommandBufferHandle<GraphicsBackend::Vulkan> commands, ImageLayout<B> layout);
    
private:

    void deleteInitialData();

    // todo: mipmaps
    TextureCreateDesc<B> myDesc = {};
	ImageHandle<B> myImage = 0;
	AllocationHandle<B> myImageMemory = 0;
    ImageLayout<B> myImageLayout = {};
    //
};
