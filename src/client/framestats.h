#pragma once

#include <chrono>
#include <cstdint>
#include <utility>
#include <vector>

namespace client::inline SPEEDO_GRAPHICS_API_NAMESPACE
{

// the frames' statistics, over (at least) half a second at a time: the frame rate, and the gpu submits (and their
// batches) per frame, which the ui shows (gFramesPerSecond, gSubmitsPerFrame, gSubmitBatchesPerFrame); how long the window
// took to show its first frame; and with SPEEDO_GPU_TIMINGS=1 each half second's frame time, whose medians Shutdown prints.
// draw thread
class FrameStats
{
public:
	// a frame submitted (gDrawnFrameCount counts it); warming: the lighting warming up in it (Renderer::IsLightingWarmingUp)
	void Count(bool warming);
	// the frame times' medians (SPEEDO_GPU_TIMINGS=1): the settled frames', and the warm-up's
	void PrintFrameTimes();

private:
	bool myStarted = false;
	std::chrono::steady_clock::time_point myTime;
	uint32_t myFrameCount = 0;
	uint64_t mySubmitCount = 0;
	uint64_t mySubmitBatchCount = 0;
	bool myWarming = false;
	// each half second's frame time (milliseconds), and whether the lighting warmed up in it. unthrottled
	// (SPEEDO_PRESENT_MODE=immediate) the gpu's whole frame, where a backend doesn't time every pass (KosmicKrisp, see "GPU
	// timings" in CLAUDE.md)
	std::vector<std::pair<float, bool>> myFrameTimes;
};

} // namespace client::inline SPEEDO_GRAPHICS_API_NAMESPACE
