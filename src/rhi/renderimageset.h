#pragma once

#include <rhi/image.h>
#include <rhi/rendertarget.h>
#include <rhi/types.h>

#include <memory>

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
	explicit RenderImageSet(Images&&... images);
	RenderImageSet(RenderImageSet<G>&& other) noexcept;
	virtual ~RenderImageSet();

	[[maybe_unused]] RenderImageSet& operator=(RenderImageSet<G>&& other) noexcept;

	void Swap(RenderImageSet& rhs) noexcept;
	friend void Swap(RenderImageSet& lhs, RenderImageSet& rhs) noexcept { lhs.Swap(rhs); }

	[[nodiscard]] virtual ImageLayout<G> GetLayout(uint32_t index) const final;

	[[nodiscard]] size_t GetImageCount() const noexcept { return myImageCount; }
	[[nodiscard]] auto GetImage(size_t index) const { ENSURE(index < GetImageCount()); return std::shared_ptr<const Image<G>>(myImages, &myImages.get()[index]); }
	void SetImage(size_t index, Image<G>&& image) { ENSURE(index < GetImageCount()); myImages.get()[index] = std::forward<Image<G>>(image); }

	virtual void End(CommandBufferHandle<G> cmd);

	virtual void Transition(CommandBufferHandle<G> cmd, ImageLayout<G> layout, ImageAspectFlags<G> aspectFlags, uint32_t index);

private:
	std::shared_ptr<Image<G>[]> myImages; //NOLINT(modernize-avoid-c-arrays)
	size_t myImageCount{};
};

} // namespace rhi

#include "renderimageset.inl"
