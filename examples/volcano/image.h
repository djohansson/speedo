#pragma once

#include "command.h"
#include "device.h"
#include "types.h"

#include <memory>
#include <filesystem>
#include <tuple>

template <GraphicsBackend B>
struct ImageCreateDesc : DeviceResourceCreateDesc<B>
{
    Extent2d<B> extent = {};
    Format<B> format = {};
    Flags<B> usage = 0;
    uint32_t mipLevels = 1;
};

template <GraphicsBackend B>
class ImageView;

template <GraphicsBackend B>
class RenderImageSet;

template <GraphicsBackend B>
class Image : public DeviceResource<B>
{
public:

    Image(Image&& other) = default;
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
    ~Image();

    Image& operator=(Image&& other) = default;

    const auto& getDesc() const { return myDesc; }
    const auto& getImageHandle() const { return std::get<0>(myData); }
    const auto& getImageMemory() const { return std::get<1>(myData); }
    const auto& getImageLayout() const { return std::get<2>(myData); }

    void transition(CommandBufferHandle<B> cmd, ImageLayout<B> layout);
    
private:

    friend class RenderImageSet<B>;

    Image( // uses provided image
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        std::tuple<ImageCreateDesc<B>, ImageHandle<B>, AllocationHandle<B>, ImageLayout<B>>&& descAndData);

    // todo: mipmaps
    const ImageCreateDesc<B> myDesc = {};
    std::tuple<ImageHandle<B>, AllocationHandle<B>, ImageLayout<B>> myData = {};
    //
};

template <GraphicsBackend B>
class ImageView : public DeviceResource<B>
{
public:
    
    ImageView(ImageView&& other) = default;
    ImageView( // creates a view from image
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        const Image<B>& image,
        Flags<Vk> aspectFlags);
    ~ImageView();

    ImageView& operator=(ImageView&& other) = default;

    auto getImageViewHandle() const { return myImageView; }

private:

    ImageView( // uses provided image view
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        ImageViewHandle<B>&& imageView);

    ImageViewHandle<B> myImageView = 0;
};
