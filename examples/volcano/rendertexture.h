#pragma once

#include "gfx-types.h"
#include "rendertarget.h"
#include "texture.h"


template <GraphicsBackend B>
class RenderTexture : public RenderTargetImpl<RenderTargetCreateDesc<B>, B>
{
	using BaseType = RenderTargetImpl<RenderTargetCreateDesc<B>, B>;

public:

	RenderTexture(RenderTexture<B>&& other) : BaseType(std::move(other)) {}
    RenderTexture(RenderTargetCreateDesc<B>&& desc) : BaseType(std::move(desc)) {}
    RenderTexture(
        std::shared_ptr<DeviceContext<B>> deviceContext,
        RenderPassHandle<B> renderPass,
        const std::vector<std::shared_ptr<Texture<B>>>& colorTextures,
        std::shared_ptr<Texture<B>> depthTexture = nullptr);
	virtual ~RenderTexture() {}

private:

    std::vector<std::shared_ptr<Texture<B>>> myColorTextures;
    std::shared_ptr<Texture<B>> myDepthTexture;
};

#include "rendertexture.inl"
