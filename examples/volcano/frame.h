#pragma once

#include "gfx-types.h"
#include "command.h"
#include "rendertarget.h"

#include <atomic>
#include <memory>
#include <vector>

template <GraphicsBackend B>
struct FrameCreateDesc
{
	SemaphoreHandle<B> timelineSemaphore = 0;
    std::shared_ptr<std::atomic_uint64_t> timelineValue;
	uint32_t index = 0;
	uint32_t maxCommandContextCount = 4;
};

template <GraphicsBackend B>
class Frame : public RenderTarget<B>
{
public:

	Frame(Frame<B>&& other);
    Frame(FrameCreateDesc<B>&& frameDesc, RenderTargetCreateDesc<B>&& renderTargetDesc);
	virtual ~Frame();

	static uint32_t ourDebugCount;

	const auto& getFrameDesc() const { return myDesc; }
	const auto& getFence() const { return myFence; }
	const auto& getRenderCompleteSemaphore() const { return myRenderCompleteSemaphore; }
	const auto& getNewImageAcquiredSemaphore() const { return myNewImageAcquiredSemaphore; }

	auto& commandContexts() { return myCommandContexts; }

	void waitForFence() const;

private:

	FrameCreateDesc<B> myDesc = {};
	std::vector<std::shared_ptr<CommandContext<B>>> myCommandContexts;
	FenceHandle<B> myFence = 0;
	SemaphoreHandle<B> myRenderCompleteSemaphore = 0;
	SemaphoreHandle<B> myNewImageAcquiredSemaphore = 0;
};
