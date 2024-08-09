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
		const std::vector<std::shared_ptr<Image<G>>>& images);
	RenderImageSet(RenderImageSet<G>&& other) noexcept;
	virtual ~RenderImageSet();

	RenderImageSet& operator=(RenderImageSet<G>&& other) noexcept;

	void Swap(RenderImageSet& rhs) noexcept;
	friend void Swap(RenderImageSet& lhs, RenderImageSet& rhs) noexcept { lhs.Swap(rhs); }

	[[nodiscard]] virtual ImageLayout<G> GetLayout(uint32_t index) const final;

	virtual void End(CommandBufferHandle<G> cmd);

	virtual void Transition(CommandBufferHandle<G> cmd, ImageLayout<G> layout, uint32_t index);

private:
	std::vector<std::shared_ptr<Image<G>>> myImages;
};

#include "renderimageset.inl"
