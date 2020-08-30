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
class ImageView;

template <GraphicsBackend B>
class RenderImageSet;

template <GraphicsBackend B>
class Image : public DeviceResource<B>
{
public:

    Image(Image&& other);
    Image( // creates uninitialized image
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        ImageCreateDesc<B>&& desc);
    Image( // copies the initial buffer into a new image. buffer gets garbage collected when finished copying.
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        const std::shared_ptr<CommandContext<B>>& commandContext,
        std::tuple<ImageCreateDesc<B>, BufferHandle<B>, AllocationHandle<B>>&& descAndInitialData);
    Image( // loads a file into a buffer and creates a new image from it. buffer gets garbage collected when finished copying.
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        const std::shared_ptr<CommandContext<B>>& commandContext,
        const std::filesystem::path& imageFile);
    Image( // takes ownership of provided image handle & allocation
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        std::tuple<ImageCreateDesc<B>, ImageHandle<B>, AllocationHandle<B>, ImageLayout<B>>&& descAndData);
    ~Image();

    Image& operator=(Image&& other);
    operator auto() const { return std::get<0>(myData); }

    const auto& getDesc() const { return myDesc; }
    const auto& getImageMemory() const { return std::get<1>(myData); }
    const auto& getImageLayout() const { return std::get<2>(myData); }

    void transition(CommandBufferHandle<B> cmd, ImageLayout<B> layout);
    
private:

    friend class RenderImageSet<B>;

    const ImageCreateDesc<B> myDesc = {};
    std::tuple<ImageHandle<B>, AllocationHandle<B>, ImageLayout<B>> myData = {};
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
    ~ImageView();

    ImageView& operator=(ImageView&& other);
    operator auto() const { return myImageView; }

private:

    ImageView( // uses provided image view
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        ImageViewHandle<B>&& imageView);

    ImageViewHandle<B> myImageView = {};
};

#include "image.inl"
