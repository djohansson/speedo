#pragma once

#include "gfx-types.h"
#include "command.h"
#include "rendertarget.h"

#include <atomic>
#include <memory>
#include <vector>

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

	Frame(Frame<B>&& other);
    Frame(
		const std::shared_ptr<DeviceContext<B>>& deviceContext,
		FrameCreateDesc<B>&& desc);
	virtual ~Frame();

	const auto& getFence() const { return myFence; }
	const auto& getRenderCompleteSemaphore() const { return myRenderCompleteSemaphore; }
	const auto& getNewImageAcquiredSemaphore() const { return myNewImageAcquiredSemaphore; }
	const auto& getLastSubmitTimelineValue() const { return myLastSubmitTimelineValue; }

	void waitForFence() const;
	void setLastSubmitTimelineValue(uint64_t timelineValue) { myLastSubmitTimelineValue = timelineValue; };

private:

	FenceHandle<B> myFence = 0;
	SemaphoreHandle<B> myRenderCompleteSemaphore = 0;
	SemaphoreHandle<B> myNewImageAcquiredSemaphore = 0;
	uint64_t myLastSubmitTimelineValue = 0;
};

#include "frame.inl"
#include "frame-vulkan.inl"
