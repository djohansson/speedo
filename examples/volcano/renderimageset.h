#pragma once

#include "gfx-types.h"
#include "rendertarget.h"
#include "image.h"


template <GraphicsBackend B>
class RenderImageSet : public RenderTargetImpl<RenderTargetCreateDesc<B>, B>
{
	using BaseType = RenderTargetImpl<RenderTargetCreateDesc<B>, B>;

public:

	RenderImageSet(RenderImageSet<B>&& other) : BaseType(std::move(other)) {}
    RenderImageSet(
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        RenderTargetCreateDesc<B>&& desc) : BaseType(deviceContext, std::move(desc)) {}
    RenderImageSet(
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        const char* name,
        const std::vector<std::shared_ptr<Image<B>>>& colorImages,
        std::shared_ptr<Image<B>> depthStencilImage = nullptr);
    RenderImageSet(
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        const char* name,
        std::vector<std::shared_ptr<Image<B>>>&& colorImages,
        std::shared_ptr<Image<B>>&& depthStencilImage = nullptr);
	virtual ~RenderImageSet();

    const auto& getColorImages() const { return myColorImages; }
    const auto& getDepthStencilImages() const { return myDepthStencilImage; }

protected:

    virtual ImageLayout<B> getColorImageLayout(uint32_t index) const final;
    virtual ImageLayout<B> getDepthStencilImageLayout() const final;

private:

    std::vector<std::shared_ptr<Image<B>>> myColorImages;
    std::shared_ptr<Image<B>> myDepthStencilImage;
};

#include "renderimageset.inl"
