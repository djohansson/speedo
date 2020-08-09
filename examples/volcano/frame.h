#pragma once

#include "command.h"
#include "rendertarget.h"
#include "types.h"

#include <memory>

template <GraphicsBackend B>
struct FrameCreateDesc : RenderTargetCreateDesc<B>
{
	uint32_t index = 0;
};

template <GraphicsBackend B>
class SwapchainContext;

template <GraphicsBackend B>
class Frame : public RenderTargetImpl<FrameCreateDesc<B>, B>
{
	using BaseType = RenderTargetImpl<FrameCreateDesc<B>, B>;

public:

	Frame(Frame<B>&& other) = default;
    Frame(const std::shared_ptr<DeviceContext<B>>& deviceContext, FrameCreateDesc<B>&& desc);
	virtual ~Frame();

	Frame& operator=(Frame&& other) = default;

	virtual ImageLayout<B> getColorImageLayout(uint32_t index) const final;
    virtual ImageLayout<B> getDepthStencilImageLayout() const final;

    virtual void end(CommandBufferHandle<B> cmd) final;

	virtual void transitionColor(CommandBufferHandle<B> cmd, ImageLayout<B> layout, uint32_t index) final;
    virtual void transitionDepth(CommandBufferHandle<B> cmd, ImageLayout<B> layout) final;

	const auto& getRenderCompleteSemaphore() const { return myRenderCompleteSemaphore; }
	const auto& getNewImageAcquiredSemaphore() const { return myNewImageAcquiredSemaphore; }
	const auto& getLastSubmitTimelineValue() const { return myLastSubmitTimelineValue; }

private:

	friend class SwapchainContext<B>;

	SemaphoreHandle<B> myRenderCompleteSemaphore = 0;
	SemaphoreHandle<B> myNewImageAcquiredSemaphore = 0;
	ImageLayout<B> myImageLayout = {};
	uint64_t myLastSubmitTimelineValue = 0;
};

#include "frame-vulkan.inl"
