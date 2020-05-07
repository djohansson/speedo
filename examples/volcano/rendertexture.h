#pragma once

#include "gfx-types.h"
#include "rendertarget.h"
#include "texture.h"

template <GraphicsBackend B>
struct RenderTextureCreateDesc : RenderTargetCreateDesc<B>
{
    RenderTextureCreateDesc(
        std::shared_ptr<DeviceContext<B>> deviceContext,
        RenderPassHandle<B> renderPass,
        const std::vector<std::shared_ptr<Texture<B>>>& colorTextures_,
        std::shared_ptr<Texture<B>>&& depthTexture_)
        : RenderTargetCreateDesc<B>{deviceContext, renderPass}
        , colorTextures(colorTextures_)
        , depthTexture(depthTexture_)
    {
        assertf(colorTextures.size(), "colorTextures cannot be empty");

        imageExtent = colorTextures.front()->getDesc().extent;
        colorImageFormat = colorTextures.front()->getDesc().format;

        colorImages.reserve(colorTextures.size());
        for (const auto& texture : colorTextures)
        {
            assertf(imageExtent == texture->getDesc().extent, "all colorTextures needs to have same extent");
            assertf(colorImageFormat == texture->getDesc().format, "all colorTextures needs to have same format");

            colorImages.emplace_back(texture->getImage());
        }

        if (depthTexture)
        {
            depthImageFormat = depthTexture->getDesc().format;
            depthImage = depthTexture->getImage();
        }
    }

    std::vector<std::shared_ptr<Texture<B>>> colorTextures;
    std::shared_ptr<Texture<B>> depthTexture; // optional
};

template <GraphicsBackend B>
class RenderTexture : public RenderTargetImpl<RenderTextureCreateDesc<B>, B>
{
	using BaseType = RenderTargetImpl<RenderTextureCreateDesc<B>, B>;

public:

	RenderTexture(RenderTexture<B>&& other) : BaseType(std::move(other)) {}
    RenderTexture(RenderTextureCreateDesc<B>&& desc) : BaseType(std::move(desc)) {}
	virtual ~RenderTexture() {}
};
