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
	~Frame();

	[[nodiscard]] Frame& operator=(Frame&& other) noexcept;

	void Swap(Frame& rhs) noexcept;
	friend void Swap(Frame& lhs, Frame& rhs) noexcept { lhs.Swap(rhs); }

	[[nodiscard]] virtual ImageLayout<G> GetColorImageLayout(uint32_t index) const;
	[[nodiscard]] virtual ImageLayout<G> GetDepthStencilImageLayout() const;

	virtual void End(CommandBufferHandle<G> cmd);

	virtual void TransitionColor(CommandBufferHandle<G> cmd, ImageLayout<G> layout, uint32_t index);
	virtual void TransitionDepthStencil(CommandBufferHandle<G> cmd, ImageLayout<G> layout);

	[[nodiscard]] QueuePresentInfo<G> PreparePresent(const QueueHostSyncInfo<G>& hostSyncInfo);

	[[nodiscard]] auto& GetFence() noexcept { return myFence; }
	[[nodiscard]] const auto& GetFence() const noexcept { return myFence; }

private:
	Fence<G> myFence{};
	ImageLayout<G> myImageLayout{};
};
