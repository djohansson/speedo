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

	ImageLayout<G> GetLayout(uint32_t) const final;

	void End(CommandBufferHandle<G> cmd) final;

	void Transition(CommandBufferHandle<G> cmd, ImageLayout<G> layout, uint32_t index) final;
	
	[[nodiscard]] QueuePresentInfo<G> PreparePresent(QueueHostSyncInfo<kVk>&& hostSyncInfo);

	[[nodiscard]] auto& GetFence() noexcept { return myFence; }
	[[nodiscard]] const auto& GetFence() const noexcept { return myFence; }
	[[nodiscard]] auto& GetSemaphore() noexcept { return mySemaphore; }
	[[nodiscard]] const auto& GetSemaphore() const noexcept { return mySemaphore; }

private:
	Fence<G> myFence{};
	Semaphore<G> mySemaphore{};
	ImageLayout<G> myImageLayout{};
};
