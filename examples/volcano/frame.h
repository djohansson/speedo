#pragma once

#include "gfx-types.h"
#include "command.h"
#include "rendertarget.h"

#include <atomic>
#include <memory>
#include <vector>

template <GraphicsBackend B>
struct FrameDesc
{
	uint32_t index = 0;
	uint32_t maxCommandContextCount = 4;
	SemaphoreHandle<B> timelineSemaphore = 0;
    std::shared_ptr<std::atomic_uint64_t> timelineValue;
};

template <GraphicsBackend B>
class Frame : public RenderTarget<B>
{
public:

	Frame(Frame<B>&& other);
    Frame(RenderTargetDesc<B>&& renderTargetDesc, FrameDesc<B>&& frameDesc);
	virtual ~Frame();

	static uint32_t ourDebugCount;

	const auto& getFrameDesc() const { return myFrameDesc; }
	const auto& getFence() const { return myFence; }
	const auto& getRenderCompleteSemaphore() const { return myRenderCompleteSemaphore; }
	const auto& getNewImageAcquiredSemaphore() const { return myNewImageAcquiredSemaphore; }

	auto& commandContexts() { return myCommandContexts; }

	void waitForFence() const;

private:

	FrameDesc<B> myFrameDesc = {};
	std::vector<std::shared_ptr<CommandContext<B>>> myCommandContexts;
	FenceHandle<B> myFence = 0;
	SemaphoreHandle<B> myRenderCompleteSemaphore = 0;
	SemaphoreHandle<B> myNewImageAcquiredSemaphore = 0;
};
