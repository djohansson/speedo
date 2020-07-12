namespace rendertexture
{

template <GraphicsBackend B>
RenderTargetCreateDesc<B> createRenderTargetCreateDesc(
    const char* name,
    const std::vector<std::shared_ptr<Texture<B>>>& colorTextures,
    const std::shared_ptr<Texture<B>>& depthStencilTexture)
{
    RenderTargetCreateDesc<B> outDesc = {};

    assertf(colorTextures.size(), "colorTextures cannot be empty");

    auto firstColorTextureExtent = colorTextures.front()->getDesc().extent;

    outDesc.name = name;
    outDesc.imageExtent = {firstColorTextureExtent.width, firstColorTextureExtent.height };
    outDesc.colorImageFormats.reserve(colorTextures.size());
    outDesc.colorImageLayouts.reserve(colorTextures.size());
    outDesc.colorImages.reserve(colorTextures.size());
    
    for (const auto& texture : colorTextures)
    {
        assertf(outDesc.imageExtent.width == texture->getDesc().extent.width, "all colorTextures needs to have same width");
        assertf(outDesc.imageExtent.height == texture->getDesc().extent.height, "all colorTextures needs to have same height");

        outDesc.colorImageFormats.emplace_back(texture->getDesc().format);
        outDesc.colorImageLayouts.emplace_back(texture->getImageLayout());
        outDesc.colorImages.emplace_back(texture->getImage());
    }

    if (depthStencilTexture)
    {
        outDesc.depthStencilImageFormat = depthStencilTexture->getDesc().format;
        outDesc.depthStencilImageLayout = depthStencilTexture->getImageLayout();
        outDesc.depthStencilImage = depthStencilTexture->getImage();
    }

    outDesc.layerCount = 1;

    return outDesc;
}

}

template <GraphicsBackend B>
RenderTexture<B>::RenderTexture(
    const std::shared_ptr<DeviceContext<B>>& deviceContext,
    const char* name,
    const std::vector<std::shared_ptr<Texture<B>>>& colorTextures,
    std::shared_ptr<Texture<B>> depthStencilTexture)
: BaseType(deviceContext, rendertexture::createRenderTargetCreateDesc(name, colorTextures, depthStencilTexture))
, myColorTextures(colorTextures)
, myDepthStencilTexture(depthStencilTexture)
{
}

template <GraphicsBackend B>
RenderTexture<B>::RenderTexture(
    const std::shared_ptr<DeviceContext<B>>& deviceContext,
    const char* name,
    std::vector<std::shared_ptr<Texture<B>>>&& colorTextures,
    std::shared_ptr<Texture<B>>&& depthStencilTexture)
: BaseType(deviceContext, rendertexture::createRenderTargetCreateDesc(name, colorTextures, depthStencilTexture))
, myColorTextures(std::move(colorTextures))
, myDepthStencilTexture(std::move(depthStencilTexture))
{
}

template <GraphicsBackend B>
RenderTexture<B>::~RenderTexture()
{
}

template <GraphicsBackend B>
ImageLayout<B> RenderTexture<B>::getColorImageLayout(uint32_t index) const
{
    return myColorTextures[index]->getImageLayout();
}

template <GraphicsBackend B>
ImageLayout<B> RenderTexture<B>::getDepthStencilImageLayout() const
{
    return myDepthStencilTexture->getImageLayout();
}
