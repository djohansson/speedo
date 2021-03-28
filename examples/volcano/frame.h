#pragma once

#include "queue.h"
#include "rendertarget.h"
#include "types.h"

#include <memory>

template <GraphicsBackend B>
struct FrameCreateDesc : RenderTargetCreateDesc<B>
{
	uint32_t index = 0;
};

template <GraphicsBackend B>
class Frame : public RenderTargetImpl<FrameCreateDesc<B>, B>
{
	using BaseType = RenderTargetImpl<FrameCreateDesc<B>, B>;

public:

	constexpr Frame() noexcept = default;
    Frame(const std::shared_ptr<DeviceContext<B>>& deviceContext, FrameCreateDesc<B>&& desc);
	Frame(Frame<B>&& other) noexcept;
	virtual ~Frame();

	Frame& operator=(Frame&& other) noexcept;

	void swap(Frame& rhs) noexcept;
    friend void swap(Frame& lhs, Frame& rhs) noexcept { lhs.swap(rhs); }

	virtual ImageLayout<B> getColorImageLayout(uint32_t index) const final;
    virtual ImageLayout<B> getDepthStencilImageLayout() const final;

    virtual void end(CommandBufferHandle<B> cmd) final;

	virtual void transitionColor(CommandBufferHandle<B> cmd, ImageLayout<B> layout, uint32_t index) final;
    virtual void transitionDepthStencil(CommandBufferHandle<B> cmd, ImageLayout<B> layout) final;

	const auto& getRenderCompleteSemaphore() const noexcept { return myRenderCompleteSemaphore; }
	const auto& getNewImageAcquiredSemaphore() const noexcept { return myNewImageAcquiredSemaphore; }
	const auto& getLastPresentTimelineValue() const noexcept { return myLastPresentTimelineValue; }

	QueuePresentInfo<B> preparePresent(uint64_t timelineValue);

private:

	SemaphoreHandle<B> myRenderCompleteSemaphore = {};
	SemaphoreHandle<B> myNewImageAcquiredSemaphore = {};
	ImageLayout<B> myImageLayout = {};
	uint64_t myLastPresentTimelineValue = 0;
};

