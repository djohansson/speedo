namespace renderimageset
{

template <GraphicsBackend B>
RenderTargetCreateDesc<B> createRenderTargetCreateDesc(
    const std::vector<std::shared_ptr<Image<B>>>& colorImages,
    const std::shared_ptr<Image<B>>& depthStencilImage)
{
    RenderTargetCreateDesc<B> outDesc = {};

    assertf(colorImages.size(), "colorImages cannot be empty");

    auto firstColorImageExtent = colorImages.front()->getDesc().mipLevels[0].extent;

    outDesc.extent = {firstColorImageExtent.width, firstColorImageExtent.height };
    outDesc.colorImageFormats.reserve(colorImages.size());
    outDesc.colorImageLayouts.reserve(colorImages.size());
    outDesc.colorImages.reserve(colorImages.size());
    
    for (const auto& image : colorImages)
    {
        assertf(outDesc.extent.width == image->getDesc().mipLevels[0].extent.width, "all colorImages needs to have same width");
        assertf(outDesc.extent.height == image->getDesc().mipLevels[0].extent.height, "all colorImages needs to have same height");

        outDesc.colorImageFormats.emplace_back(image->getDesc().format);
        outDesc.colorImageLayouts.emplace_back(image->getImageLayout());
        outDesc.colorImages.emplace_back(*image);
    }

    if (depthStencilImage)
    {
        outDesc.depthStencilImageFormat = depthStencilImage->getDesc().format;
        outDesc.depthStencilImageLayout = depthStencilImage->getImageLayout();
        outDesc.depthStencilImage = *depthStencilImage;
    }

    outDesc.layerCount = 1;

    return outDesc;
}

}

template <GraphicsBackend B>
RenderImageSet<B>::RenderImageSet(
    const std::shared_ptr<DeviceContext<B>>& deviceContext,
    const std::vector<std::shared_ptr<Image<B>>>& colorImages,
    const std::shared_ptr<Image<B>>& depthStencilImage)
: BaseType(deviceContext, renderimageset::createRenderTargetCreateDesc(colorImages, depthStencilImage))
, myColorImages(colorImages)
, myDepthStencilImage(depthStencilImage)
{
}

template <GraphicsBackend B>
RenderImageSet<B>::RenderImageSet(RenderImageSet&& other) noexcept
: BaseType(std::move(other))
, myColorImages(std::exchange(other.myColorImages, {}))
, myDepthStencilImage(std::exchange(other.myDepthStencilImage, {}))
{
}

template <GraphicsBackend B>
RenderImageSet<B>::~RenderImageSet()
{
}

template <GraphicsBackend B>
RenderImageSet<B>& RenderImageSet<B>::operator=(RenderImageSet&& other) noexcept
{
    BaseType::operator=(std::move(other));
	myColorImages = std::exchange(other.myColorImages, {});
    myDepthStencilImage = std::exchange(other.myDepthStencilImage, {});
    return *this;
}

template <GraphicsBackend B>
void RenderImageSet<B>::swap(RenderImageSet& rhs) noexcept
{
    BaseType::swap(rhs);
	std::swap(myColorImages, rhs.myColorImages);
    std::swap(myDepthStencilImage, rhs.myDepthStencilImage);
}

template <GraphicsBackend B>
ImageLayout<B> RenderImageSet<B>::getColorImageLayout(uint32_t index) const
{
    return myColorImages[index]->getImageLayout();
}

template <GraphicsBackend B>
ImageLayout<B> RenderImageSet<B>::getDepthStencilImageLayout() const
{
    return myDepthStencilImage->getImageLayout();
}

template <GraphicsBackend B>
void RenderImageSet<B>::end(CommandBufferHandle<B> cmd)
{
    RenderTarget<B>::end(cmd);
    
    uint32_t imageIt = 0ul;
    for (; imageIt < myColorImages.size(); imageIt++)
        myColorImages[imageIt]->setImageLayout(this->getAttachmentDesc(imageIt).finalLayout);

    if (myDepthStencilImage)
        myDepthStencilImage->setImageLayout(this->getAttachmentDesc(imageIt).finalLayout);
}

template <GraphicsBackend B>
void RenderImageSet<B>::transitionColor(CommandBufferHandle<B> cmd, ImageLayout<B> layout, uint32_t index)
{
    myColorImages[index]->transition(cmd, layout);
}

template <GraphicsBackend B>
void RenderImageSet<B>::transitionDepthStencil(CommandBufferHandle<B> cmd, ImageLayout<B> layout)
{
    myDepthStencilImage->transition(cmd, layout);
}
