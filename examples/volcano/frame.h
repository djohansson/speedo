#pragma once

#include "gfx-types.h"
#include "command.h"
#include "rendertarget.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <vector>

template <GraphicsBackend B>
class Frame : public RenderTarget<B>
{
public:

	Frame(Frame<B>&& other)
    : RenderTarget<B>(std::move(other))
	, myCommands(std::move(other.myCommands))
	, myFence(other.myFence)
	, myRenderCompleteSemaphore(other.myRenderCompleteSemaphore)
	, myNewImageAcquiredSemaphore(other.myNewImageAcquiredSemaphore)
	, myTimelineSemaphore(other.myTimelineSemaphore)
    , myTimelineValue(std::move(other.myTimelineValue))
	, myTimestamp(other.myTimestamp)
    {
		other.myFence = 0;
		other.myRenderCompleteSemaphore = 0;
		other.myNewImageAcquiredSemaphore = 0;
		other.myTimelineSemaphore = 0;
		other.myTimelineValue.reset();
		other.myTimestamp = std::chrono::high_resolution_clock::time_point();
    }
	Frame(RenderTargetDesc<B>&& desc);
	virtual ~Frame();

	const auto& getFence() const { return myFence; }
	const auto& getRenderCompleteSemaphore() const { return myRenderCompleteSemaphore; }
	const auto& getNewImageAcquiredSemaphore() const { return myNewImageAcquiredSemaphore; }
	const auto& getTimestamp() const { return myTimestamp; }

	auto& commands() { return myCommands; }

	void waitForFence();

private:

	std::vector<CommandContext<B>> myCommands;
	FenceHandle<B> myFence = 0;
	SemaphoreHandle<B> myRenderCompleteSemaphore = 0;
	SemaphoreHandle<B> myNewImageAcquiredSemaphore = 0;
	SemaphoreHandle<B> myTimelineSemaphore = 0;
    std::shared_ptr<std::atomic_uint64_t> myTimelineValue;
	std::chrono::high_resolution_clock::time_point myTimestamp;
};
