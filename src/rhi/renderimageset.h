#pragma once

#include "image.h"
#include "rendertarget.h"
#include "types.h"

#include <memory>
#include <vector>

template <GraphicsApi G>
class RenderImageSet final : public RenderTargetImpl<RenderTargetCreateDesc<G>, G>
{
	using BaseType = RenderTargetImpl<RenderTargetCreateDesc<G>, G>;

public:
	constexpr RenderImageSet() noexcept = default;
	RenderImageSet(
		const std::shared_ptr<Device<G>>& device,
		const std::vector<std::shared_ptr<Image<G>>>& colorImages,
		const std::shared_ptr<Image<G>>& depthStencilImage = {});
	RenderImageSet(RenderImageSet<G>&& other) noexcept;
	virtual ~RenderImageSet();

	RenderImageSet& operator=(RenderImageSet<G>&& other) noexcept;

	void swap(RenderImageSet& rhs) noexcept;
	friend void swap(RenderImageSet& lhs, RenderImageSet& rhs) noexcept { lhs.swap(rhs); }

	virtual ImageLayout<G> getColorImageLayout(uint32_t index) const;
	virtual ImageLayout<G> getDepthStencilImageLayout() const;

	virtual void end(CommandBufferHandle<G> cmd);

	virtual void
	transitionColor(CommandBufferHandle<G> cmd, ImageLayout<G> layout, uint32_t index);
	virtual void transitionDepthStencil(CommandBufferHandle<G> cmd, ImageLayout<G> layout);

private:
	std::vector<std::shared_ptr<Image<G>>> myColorImages;
	std::shared_ptr<Image<G>> myDepthStencilImage;
};

#include "renderimageset.inl"
