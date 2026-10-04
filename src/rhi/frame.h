#pragma once

#include <rhi/queue.h>
#include <rhi/rendertarget.h>
#include <rhi/types.h>

#include <cstdint>

namespace rhi
{

template <GraphicsApi G>
class Frame;

template <GraphicsApi G>
struct FrameCreateDesc final : RenderTargetCreateDesc<G>
{
	uint32_t index = 0;
};

template <GraphicsApi G>
struct ObjectTraits<RenderTarget<Frame<G>, G>>
{
	using CreateDescType = RenderTargetCreateDesc<G>;
};

template <GraphicsApi G>
struct ObjectTraits<Frame<G>>
{
	using CreateDescType = FrameCreateDesc<G>;
};

template <GraphicsApi G>
class Frame final : public RenderTarget<Frame<G>, G> // todo: second G is redundant, can be obtained from ObjectTraits<Frame<G>>::CreateDescType::GetApi()
{
public:
	using SuperType = RenderTarget<Frame<G>, G>;
	using CreateDescType = ObjectTraits<Frame<G>>::CreateDescType;

	constexpr Frame() noexcept = default;
	explicit Frame(CreateDescType&& desc);
	Frame(Frame<G>&& other) noexcept;

	[[maybe_unused]] Frame& operator=(Frame&& other) noexcept;

	void Swap(Frame& rhs) noexcept;
	friend void Swap(Frame& lhs, Frame& rhs) noexcept { lhs.Swap(rhs); }

	[[nodiscard]] ImageLayout GetLayout(uint32_t) const final;

	void End(CommandBufferHandle<G> cmd) final;

	void Transition(CommandBufferHandle<G> cmd, ImageLayout layout, ImageAspect aspectFlags, uint32_t index) final;
	
	[[nodiscard]] QueuePresentInfo<G> PreparePresent();

private:
	ImageLayout myImageLayout{}; // replace with layout in desc?
};

} // namespace rhi
