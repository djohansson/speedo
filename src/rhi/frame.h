#pragma once

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
	virtual ~Frame();

	Frame& operator=(Frame&& other) noexcept;

	void swap(Frame& rhs) noexcept;
	friend void swap(Frame& lhs, Frame& rhs) noexcept { lhs.swap(rhs); }

	virtual ImageLayout<G> getColorImageLayout(uint32_t index) const;
	virtual ImageLayout<G> getDepthStencilImageLayout() const;

	virtual void end(CommandBufferHandle<G> cmd);

	virtual void transitionColor(CommandBufferHandle<G> cmd, ImageLayout<G> layout, uint32_t index);
	virtual void transitionDepthStencil(CommandBufferHandle<G> cmd, ImageLayout<G> layout);

	const auto& getRenderCompleteSemaphore() const noexcept { return myRenderCompleteSemaphore; }
	const auto& getNewImageAcquiredSemaphore() const noexcept { return myNewImageAcquiredSemaphore; }
	const auto& getLastPresentSyncInfo() const noexcept { return myLastPresentSyncInfo; }

	QueuePresentInfo<G> preparePresent(const QueueHostSyncInfo<G>& hostSyncInfo);

private:
	SemaphoreHandle<G> myRenderCompleteSemaphore{};
	SemaphoreHandle<G> myNewImageAcquiredSemaphore{};
	ImageLayout<G> myImageLayout{};
	QueueHostSyncInfo<G> myLastPresentSyncInfo{};
};
