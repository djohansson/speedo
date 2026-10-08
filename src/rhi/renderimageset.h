#pragma once

#include <rhi/image.h>
#include <rhi/rendertarget.h>
#include <rhi/types.h>

#include <concepts>
#include <format>
#include <memory>
#include <vector>

namespace rhi
{

template <GraphicsApi G>
class RenderImageSet;

template <GraphicsApi G>
struct ObjectTraits<RenderImageSet<G>>
{
	using CreateDescType = RenderTargetCreateDesc<G>;
};

template <GraphicsApi G>
class RenderImageSet final : public RenderTarget<RenderImageSet<G>, G>
{
	using SuperType = RenderTarget<RenderImageSet<G>, G>;
	
public:
	constexpr RenderImageSet() noexcept = default;
	template <typename... Images>
		requires(std::same_as<std::remove_cvref_t<Images>, Image<G>> && ...)
	explicit RenderImageSet(Images&&... images); // takes the images
	explicit RenderImageSet(std::vector<std::shared_ptr<Image<G>>> images); // shares them (e.g. with a frame graph)
	RenderImageSet(RenderImageSet<G>&& other) noexcept;
	virtual ~RenderImageSet();

	[[maybe_unused]] RenderImageSet& operator=(RenderImageSet<G>&& other) noexcept;

	void Swap(RenderImageSet& rhs) noexcept;
	friend void Swap(RenderImageSet& lhs, RenderImageSet& rhs) noexcept { lhs.Swap(rhs); }

	[[nodiscard]] virtual ImageLayout GetLayout(uint32_t index) const final;

	[[nodiscard]] size_t GetImageCount() const noexcept { return myImages.size(); }
	[[nodiscard]] std::shared_ptr<const Image<G>> GetImage(size_t index) const { ENSURE(index < GetImageCount()); return myImages[index]; }

	virtual void End(CommandBufferHandle<G> cmd);

	virtual void Transition(CommandBufferHandle<G> cmd, ImageLayout layout, ImageAspect aspectFlags, uint32_t index);

private:
	std::vector<std::shared_ptr<Image<G>>> myImages;
};

} // namespace rhi

#include "renderimageset.inl"
