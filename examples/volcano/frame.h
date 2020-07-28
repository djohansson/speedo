#pragma once

#include "command.h"
#include "rendertarget.h"
#include "types.h"

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

	Frame(Frame<B>&& other) = default;
    Frame(const std::shared_ptr<DeviceContext<B>>& deviceContext, FrameCreateDesc<B>&& desc);
	virtual ~Frame();

	const auto& getRenderCompleteSemaphore() const { return myRenderCompleteSemaphore; }
	const auto& getNewImageAcquiredSemaphore() const { return myNewImageAcquiredSemaphore; }
	const auto& getLastSubmitTimelineValue() const { return myLastSubmitTimelineValue; }

	void setLastSubmitTimelineValue(uint64_t timelineValue) { myLastSubmitTimelineValue = timelineValue; };

protected:

    virtual ImageLayout<B> getColorImageLayout(uint32_t index) const final;
    virtual ImageLayout<B> getDepthStencilImageLayout() const final;

private:

	SemaphoreHandle<B> myRenderCompleteSemaphore = 0;
	SemaphoreHandle<B> myNewImageAcquiredSemaphore = 0;
	ImageLayout<B> myImageLayout = {};
	uint64_t myLastSubmitTimelineValue = 0;
};

#include "frame-vulkan.inl"
