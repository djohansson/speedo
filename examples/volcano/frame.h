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
	SemaphoreHandle<B> timelineSemaphore = 0;
    std::shared_ptr<std::atomic_uint64_t> timelineValue;
	uint32_t index = 0;
};

template <GraphicsBackend B>
class Frame : public RenderTargetImpl<FrameCreateDesc<B>, B>
{
	using BaseType = RenderTargetImpl<FrameCreateDesc<B>, B>;

public:

	Frame(Frame<B>&& other);
    Frame(FrameCreateDesc<B>&& desc);
	virtual ~Frame();

	static uint32_t ourDebugCount;

	const auto& getFence() const { return myFence; }
	const auto& getRenderCompleteSemaphore() const { return myRenderCompleteSemaphore; }
	const auto& getNewImageAcquiredSemaphore() const { return myNewImageAcquiredSemaphore; }

	void waitForFence() const;

private:

	FenceHandle<B> myFence = 0;
	SemaphoreHandle<B> myRenderCompleteSemaphore = 0;
	SemaphoreHandle<B> myNewImageAcquiredSemaphore = 0;
};

#include "frame-vulkan.h"
