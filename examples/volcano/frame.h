#pragma once

#include "gfx-types.h"
#include "command.h"
#include "utils.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <vector>

template <GraphicsBackend B>
struct FrameCreateDesc
{
	DeviceHandle<B> device = 0;
};

template <GraphicsBackend B>
struct Frame : Noncopyable
{
	Frame() = default;
	Frame(Frame<B>&& other)
    : myDesc(other.myDesc)
	, index(other.index)
    , frameBuffer(other.frameBuffer)
	, commands(std::move(other.commands))
	, fence(other.fence)
	, renderCompleteSemaphore(other.renderCompleteSemaphore)
	, newImageAcquiredSemaphore(other.newImageAcquiredSemaphore)
	, timelineSemaphore(other.timelineSemaphore)
    , timelineValue(std::move(other.timelineValue))
	, graphicsFrameTimestamp(other.graphicsFrameTimestamp)
	, graphicsDeltaTime(other.graphicsDeltaTime)
    {
		other.myDesc = {};
		other.index = 0;
		other.frameBuffer = 0;
		other.fence = 0;
		other.renderCompleteSemaphore = 0;
		other.newImageAcquiredSemaphore = 0;
		other.timelineSemaphore = 0;
    }
	Frame(FrameCreateDesc<B>&& desc);
	~Frame();

	FrameCreateDesc<B> myDesc = {};
	
	uint64_t index = 0;
	
	FramebufferHandle<B> frameBuffer = 0;
	std::vector<CommandContext<B>> commands; // count = [threadCount]
	
	FenceHandle<B> fence = 0;
	SemaphoreHandle<B> renderCompleteSemaphore = 0;
	SemaphoreHandle<B> newImageAcquiredSemaphore = 0;
	SemaphoreHandle<B> timelineSemaphore = 0;
    std::shared_ptr<std::atomic_uint64_t> timelineValue;

	std::chrono::high_resolution_clock::time_point graphicsFrameTimestamp;
	std::chrono::duration<double> graphicsDeltaTime;
};
