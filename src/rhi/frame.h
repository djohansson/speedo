#pragma once

#include "fence.h"
#include "queue.h"
#include "rendertarget.h"
#include "types.h"

#include <memory>

template <GraphicsApi G>
struct FrameCreateDesc : RenderTargetCreateDesc<G>
{
	uint32_t index = 0;
};

template <GraphicsApi G>
class Frame final : public RenderTargetImpl<FrameCreateDesc<G>, G>
{
	using BaseType = RenderTargetImpl<FrameCreateDesc<G>, G>;

public:
	constexpr Frame() noexcept = default;
	Frame(const std::shared_ptr<Device<G>>& device, FrameCreateDesc<G>&& desc);
	Frame(Frame<G>&& other) noexcept;
	~Frame() = default;

	[[maybe_unused]] Frame& operator=(Frame&& other) noexcept;

	void Swap(Frame& rhs) noexcept;
	friend void Swap(Frame& lhs, Frame& rhs) noexcept { lhs.Swap(rhs); }

	[[nodiscard]] ImageLayout<G> GetLayout(uint32_t) const final;

	void End(CommandBufferHandle<G> cmd) final;

	void Transition(CommandBufferHandle<G> cmd, ImageLayout<G> layout, ImageAspectFlags<G> aspectFlags, uint32_t index) final;
	
	[[nodiscard]] QueuePresentInfo<G> PreparePresent();

private:
	ImageLayout<G> myImageLayout{};
};
