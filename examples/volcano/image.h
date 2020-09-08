#pragma once

#include "command.h"
#include "device.h"
#include "types.h"

#include <memory>
#include <filesystem>
#include <tuple>

template <GraphicsBackend B>
struct ImageMipLevelDesc
{
    Extent2d<B> extent = {};
    uint32_t size = 0;
    uint32_t offset = 0;
};

template <GraphicsBackend B>
struct ImageCreateDesc : DeviceResourceCreateDesc<B>
{
    std::vector<ImageMipLevelDesc<B>> mipLevels;
    Format<B> format = {};
    Flags<B> usage = {};
};

template <GraphicsBackend B>
class Image : public DeviceResource<B>
{
    using ValueType = std::tuple<ImageHandle<B>, AllocationHandle<B>, ImageLayout<B>>;

public:

    Image(Image&& other);
    Image( // creates uninitialized image
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        ImageCreateDesc<B>&& desc);
    Image( // copies the initial buffer into a new image. buffer gets garbage collected when finished copying.
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        const std::shared_ptr<CommandContext<B>>& commandContext,
        std::tuple<ImageCreateDesc<Vk>, BufferHandle<Vk>, AllocationHandle<Vk>>&& descAndInitialData);
    Image( // loads a file into a buffer and creates a new image from it. buffer gets garbage collected when finished copying.
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        const std::shared_ptr<CommandContext<B>>& commandContext,
        const std::filesystem::path& imageFile);
    Image( // takes ownership of provided image handle & allocation
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        ImageCreateDesc<B>&& desc,
        ValueType&& data);
    ~Image();

    Image& operator=(Image&& other);
    operator auto() const { return std::get<0>(myImage); }

    const auto& getDesc() const { return myDesc; }
    const auto& getImageMemory() const { return std::get<1>(myImage); }
    const auto& getImageLayout() const { return std::get<2>(myImage); }

    void transition(CommandBufferHandle<B> cmd, ImageLayout<B> layout);

    // this method is not meant to be used except in very special cases
    // such as for instance to update the image layout after a render pass
    // (which implicitly changes the image layout).
    void setImageLayout(ImageLayout<B> layout) { std::get<2>(myImage) = layout; }
    
private:

    const ImageCreateDesc<B> myDesc = {};
    ValueType myImage = {};
};

template <GraphicsBackend B>
class ImageView : public DeviceResource<B>
{
public:
    
    ImageView(ImageView&& other);
    ImageView( // creates a view from image
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        const Image<B>& image,
        Flags<Vk> aspectFlags);
    ImageView( // uses provided image view
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        ImageViewHandle<B>&& imageView);
    ~ImageView();

    ImageView& operator=(ImageView&& other);
    operator auto() const { return myImageView; }

private:

    ImageViewHandle<B> myImageView = {};
};

#include "image.inl"
