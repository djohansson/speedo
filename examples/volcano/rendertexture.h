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
    RenderTexture(
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        RenderTargetCreateDesc<B>&& desc) : BaseType(deviceContext, std::move(desc)) {}
    RenderTexture(
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        const char* name,
        const std::vector<std::shared_ptr<Texture<B>>>& colorTextures,
        std::shared_ptr<Texture<B>> depthStencilTexture = nullptr);
    RenderTexture(
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        const char* name,
        std::vector<std::shared_ptr<Texture<B>>>&& colorTextures,
        std::shared_ptr<Texture<B>>&& depthStencilTexture = nullptr);
	virtual ~RenderTexture();

    const auto& getColorTextures() const { return myColorTextures; }
    const auto& getDepthStencilTexture() const { return myDepthStencilTexture; }

protected:

    virtual ImageLayout<B> getColorImageLayout(uint32_t index) const final;
    virtual ImageLayout<B> getDepthStencilImageLayout() const final;

private:

    std::vector<std::shared_ptr<Texture<B>>> myColorTextures;
    std::shared_ptr<Texture<B>> myDepthStencilTexture;
};

#include "rendertexture.inl"
