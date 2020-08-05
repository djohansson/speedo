#pragma once

#include "command.h"
#include "rendertarget.h"
#include "types.h"

#include <memory>

template <GraphicsBackend B>
class SwapchainContext;

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

	Frame(Frame<B>&& other) = default;
    Frame(const std::shared_ptr<DeviceContext<B>>& deviceContext, FrameCreateDesc<B>&& desc);
	virtual ~Frame();

	Frame& operator=(Frame&& other) = default;

	const auto& getRenderCompleteSemaphore() const { return myRenderCompleteSemaphore; }
	const auto& getNewImageAcquiredSemaphore() const { return myNewImageAcquiredSemaphore; }
	const auto& getLastSubmitTimelineValue() const { return myLastSubmitTimelineValue; }

protected:

    virtual ImageLayout<B> getColorImageLayout(uint32_t index) const final;
    virtual ImageLayout<B> getDepthStencilImageLayout() const final;

private:

	friend class SwapchainContext<B>;

	uint64_t submit(
		const std::shared_ptr<CommandContext<B>>& commandContext,
		SemaphoreHandle<B> waitSemaphore,
		uint64_t waitTimelineValue);

	SemaphoreHandle<B> myRenderCompleteSemaphore = 0;
	SemaphoreHandle<B> myNewImageAcquiredSemaphore = 0;
	ImageLayout<B> myImageLayout = {};
	uint64_t myLastSubmitTimelineValue = 0;
};

#include "frame-vulkan.inl"
