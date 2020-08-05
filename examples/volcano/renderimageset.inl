namespace renderimage
{

template <GraphicsBackend B>
RenderTargetCreateDesc<B> createRenderTargetCreateDesc(
    const char* name,
    const std::vector<std::shared_ptr<Image<B>>>& colorImages,
    const std::shared_ptr<Image<B>>& depthStencilImage)
{
    RenderTargetCreateDesc<B> outDesc = {};

    assertf(colorImages.size(), "colorImages cannot be empty");

    auto firstColorImageExtent = colorImages.front()->getDesc().extent;

    outDesc.name = name;
    outDesc.imageExtent = {firstColorImageExtent.width, firstColorImageExtent.height };
    outDesc.colorImageFormats.reserve(colorImages.size());
    outDesc.colorImageLayouts.reserve(colorImages.size());
    outDesc.colorImages.reserve(colorImages.size());
    
    for (const auto& image : colorImages)
    {
        assertf(outDesc.imageExtent.width == image->getDesc().extent.width, "all colorImages needs to have same width");
        assertf(outDesc.imageExtent.height == image->getDesc().extent.height, "all colorImages needs to have same height");

        outDesc.colorImageFormats.emplace_back(image->getDesc().format);
        outDesc.colorImageLayouts.emplace_back(image->getImageLayout());
        outDesc.colorImages.emplace_back(image->getImageHandle());
    }

    if (depthStencilImage)
    {
        outDesc.depthStencilImageFormat = depthStencilImage->getDesc().format;
        outDesc.depthStencilImageLayout = depthStencilImage->getImageLayout();
        outDesc.depthStencilImage = depthStencilImage->getImageHandle();
    }

    outDesc.layerCount = 1;

    return outDesc;
}

}

template <GraphicsBackend B>
RenderImageSet<B>::RenderImageSet(
    const std::shared_ptr<DeviceContext<B>>& deviceContext,
    const char* name,
    const std::vector<std::shared_ptr<Image<B>>>& colorImages,
    std::shared_ptr<Image<B>> depthStencilImage)
: BaseType(deviceContext, renderimage::createRenderTargetCreateDesc(name, colorImages, depthStencilImage))
, myColorImages(colorImages)
, myDepthStencilImage(depthStencilImage)
{
}

template <GraphicsBackend B>
RenderImageSet<B>::RenderImageSet(
    const std::shared_ptr<DeviceContext<B>>& deviceContext,
    const char* name,
    std::vector<std::shared_ptr<Image<B>>>&& colorImages,
    std::shared_ptr<Image<B>>&& depthStencilImage)
: BaseType(deviceContext, renderimage::createRenderTargetCreateDesc(name, colorImages, depthStencilImage))
, myColorImages(std::move(colorImages))
, myDepthStencilImage(std::move(depthStencilImage))
{
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
