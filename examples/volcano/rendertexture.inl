namespace rendertexture
{

template <GraphicsBackend B>
RenderTargetCreateDesc<B> createRenderTargetCreateDesc(
    const char* name,
    RenderPassHandle<B> renderPass,
    const std::vector<std::shared_ptr<Texture<B>>>& colorTextures,
    std::shared_ptr<Texture<B>> depthTexture)
{
    RenderTargetCreateDesc<B> outDesc = {};

    assertf(colorTextures.size(), "colorTextures cannot be empty");

    outDesc.name = name;
    outDesc.imageExtent = colorTextures.front()->getDesc().extent;
    outDesc.colorImageFormat = colorTextures.front()->getDesc().format;
    outDesc.colorImages.reserve(colorTextures.size());
    
    for (const auto& texture : colorTextures)
    {
        assertf(outDesc.imageExtent.width == texture->getDesc().extent.width, "all colorTextures needs to have same width");
        assertf(outDesc.imageExtent.height == texture->getDesc().extent.height, "all colorTextures needs to have same height");
        assertf(outDesc.colorImageFormat == texture->getDesc().format, "all colorTextures needs to have same format");

        outDesc.colorImages.emplace_back(texture->getImage());
    }

    if (depthTexture)
    {
        outDesc.depthImageFormat = depthTexture->getDesc().format;
        outDesc.depthImage = depthTexture->getImage();
    }

    return outDesc;
}

}

template <GraphicsBackend B>
RenderTexture<B>::RenderTexture(
    std::shared_ptr<DeviceContext<B>> deviceContext,
    const char* name,
    RenderPassHandle<B> renderPass,
    const std::vector<std::shared_ptr<Texture<B>>>& colorTextures,
    std::shared_ptr<Texture<B>> depthTexture)
: BaseType(deviceContext, rendertexture::createRenderTargetCreateDesc(name, renderPass, colorTextures, depthTexture))
, myColorTextures(colorTextures)
, myDepthTexture(depthTexture)
{
}
