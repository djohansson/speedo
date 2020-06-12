#pragma once

#include "command.h"
#include "device.h"
#include "deviceresource.h"
#include "gfx-types.h"

#include <memory>
#include <filesystem>

template <GraphicsBackend B>
struct TextureCreateDesc : DeviceResourceCreateDesc<B>
{
    Extent2d<B> extent = {};
    Format<B> format = {};
    Flags<B> usage = 0;
};

template <GraphicsBackend B>
class Texture : public DeviceResource<B>
{
public:

    Texture(Texture&& other) = default;
    Texture( // creates uninitialized texture
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        TextureCreateDesc<B>&& desc);
    Texture( // uses provided image
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        std::tuple<TextureCreateDesc<B>, ImageHandle<B>, AllocationHandle<B>, ImageLayout<B>>&& descAndData);
    Texture( // copies the initial buffer into a new image. buffer gets garbage collected when finished copying.
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        const std::shared_ptr<CommandContext<B>>& commandContext,
        std::tuple<TextureCreateDesc<B>, BufferHandle<B>, AllocationHandle<B>>&& descAndInitialData);
    Texture( // loads a file into a buffer and creates a new image from it. buffer gets garbage collected when finished copying.
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        const std::shared_ptr<CommandContext<B>>& commandContext,
        const std::filesystem::path& textureFile);
    ~Texture();

    const auto& getDesc() const { return myDesc; }
    const auto& getImage() const { return std::get<0>(myData); }
    const auto& getImageMemory() const { return std::get<1>(myData); }
    const auto& getImageLayout() const { return std::get<2>(myData); }

    // todo: create scoped wrapper for the view handle
    ImageViewHandle<B> createView(Flags<B> aspectFlags);
    //

    void transition(CommandBufferHandle<B> commands, ImageLayout<B> layout);
    
private:

    // todo: mipmaps
    const TextureCreateDesc<B> myDesc = {};
    std::tuple<ImageHandle<B>, AllocationHandle<B>, ImageLayout<B>> myData = {};
    //
};
