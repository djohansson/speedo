#pragma once

#include "gfx-types.h"
#include "command.h"

#include <chrono>
#include <memory>
#include <vector>

template <GraphicsBackend B>
struct Frame
{
	uint32_t index = 0;
	
	FramebufferHandle<B> frameBuffer = 0;
	std::vector<CommandContext<B>> commands; // count = [threadCount]
	
	FenceHandle<B> fence = 0;
	SemaphoreHandle<B> renderCompleteSemaphore = 0;
	SemaphoreHandle<B> newImageAcquiredSemaphore = 0;

	std::chrono::high_resolution_clock::time_point graphicsFrameTimestamp;
	std::chrono::duration<double> graphicsDeltaTime;
};
