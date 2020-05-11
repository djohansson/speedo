template <GraphicsBackend B>
RenderTexture<B>::RenderTexture(
    std::shared_ptr<DeviceContext<B>> deviceContext,
    RenderPassHandle<B> renderPass,
    const std::vector<std::shared_ptr<Texture<B>>>& colorTextures,
    std::shared_ptr<Texture<B>> depthTexture)
    : RenderTargetCreateDesc<B>{deviceContext, renderPass}
    , colorTextures(colorTextures_)
    , depthTexture(depthTexture_)
{
    assertf(colorTextures.size(), "colorTextures cannot be empty");

    this->imageExtent = colorTextures.front()->getDesc().extent;
    this->colorImageFormat = colorTextures.front()->getDesc().format;
    this->colorImages.reserve(colorTextures.size());
    
    for (const auto& texture : colorTextures)
    {
        assertf(this->imageExtent == texture->getDesc().extent, "all colorTextures needs to have same extent");
        assertf(this->colorImageFormat == texture->getDesc().format, "all colorTextures needs to have same format");

        this->colorImages.emplace_back(texture->getImage());
    }

    if (depthTexture)
    {
        this->depthImageFormat = depthTexture->getDesc().format;
        this->depthImage = depthTexture->getImage();
    }
}
