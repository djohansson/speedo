#pragma once

#include "rendertarget.h"
#include "image.h"
#include "types.h"

#include <vector>
#include <memory>

template <GraphicsBackend B>
class RenderImageSet : public RenderTargetImpl<RenderTargetCreateDesc<B>, B>
{
	using BaseType = RenderTargetImpl<RenderTargetCreateDesc<B>, B>;

public:

    constexpr RenderImageSet() noexcept = default;
    RenderImageSet(
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        const std::vector<std::shared_ptr<Image<B>>>& colorImages,
        const std::shared_ptr<Image<B>>& depthStencilImage = {});
    RenderImageSet(RenderImageSet<B>&& other) noexcept;
    virtual ~RenderImageSet();

    RenderImageSet& operator=(RenderImageSet<B>&& other) noexcept;

    void swap(RenderImageSet& rhs) noexcept;
	friend void swap(RenderImageSet& lhs, RenderImageSet& rhs) noexcept { lhs.swap(rhs); }

    virtual ImageLayout<B> getColorImageLayout(uint32_t index) const final;
    virtual ImageLayout<B> getDepthStencilImageLayout() const final;

    virtual void end(CommandBufferHandle<B> cmd) final;

    virtual void transitionColor(CommandBufferHandle<B> cmd, ImageLayout<B> layout, uint32_t index) final;
    virtual void transitionDepthStencil(CommandBufferHandle<B> cmd, ImageLayout<B> layout) final;

private:

    std::vector<std::shared_ptr<Image<B>>> myColorImages;
    std::shared_ptr<Image<B>> myDepthStencilImage;
};

#include "renderimageset.inl"
