#include "framestats.h"

#include "windowedapplicationdetail.h"

#include <algorithm>
#include <print>
#include <ranges>
#include <string_view>

namespace client::inline SPEEDO_GRAPHICS_API_NAMESPACE
{

namespace framestats
{

// about when the process started (its static initialization, before main), which the first frame's time counts from
const auto gStartTime = std::chrono::steady_clock::now();

// SPEEDO_GPU_TIMINGS=1: the frame times collected
const bool gCollectFrameTimes = []
{
	const char* collect = std::getenv("SPEEDO_GPU_TIMINGS");
	return collect != nullptr && std::string_view(collect) == "1";
}();

} // namespace framestats

void FrameStats::Count(bool warming)
{
	using namespace windowedapplication;
	using namespace std::chrono_literals;

	// from the first frame on
	if (!myStarted)
	{
		myStarted = true;
		myTime = std::chrono::steady_clock::now();
		mySubmitCount = Queue::GetSubmitCount();
		mySubmitBatchCount = Queue::GetSubmitBatchCount();
	}
	myFrameCount++;
	myWarming = myWarming || warming;
	// how long the window took to show its first frame (the startup's cost before anything is drawn: the ImGui renderer's
	// shaders, compiled or precompiled, and the window's pipelines)
	if (gDrawnFrameCount.fetch_add(1, std::memory_order_relaxed) == 0)
		std::println(
			"first frame presented {:.2f} s after start",
			std::chrono::duration<double>(std::chrono::steady_clock::now() - framestats::gStartTime).count());
	if (auto now = std::chrono::steady_clock::now(); now - myTime >= 500ms)
	{
		auto submitCount = Queue::GetSubmitCount();
		auto submitBatchCount = Queue::GetSubmitBatchCount();
		auto frames = static_cast<float>(myFrameCount);
		gFramesPerSecond.store(frames / std::chrono::duration<float>(now - myTime).count(), std::memory_order_relaxed);
		if (framestats::gCollectFrameTimes)
			myFrameTimes.emplace_back(std::chrono::duration<float, std::milli>(now - myTime).count() / frames, myWarming);
		gSubmitsPerFrame.store(static_cast<float>(submitCount - mySubmitCount) / frames, std::memory_order_relaxed);
		gSubmitBatchesPerFrame.store(static_cast<float>(submitBatchCount - mySubmitBatchCount) / frames, std::memory_order_relaxed);
		myFrameCount = 0;
		myWarming = false;
		myTime = now;
		mySubmitCount = submitCount;
		mySubmitBatchCount = submitBatchCount;
	}
}

void FrameStats::PrintFrameTimes()
{
	if (myFrameTimes.size() <= 2)
		return;
	// the settled frames: those after the lighting first warmed up but not warming up again (its warm-up rays cost Bistro
	// 16 ms a frame for its first 256 frames, which a run of 400 frames measured as most of its frame time), else, where
	// it never warmed up (no indirect lighting, or no surface cache), past the first half seconds' load and compile
	auto warmed = std::ranges::find_if(myFrameTimes, [](const auto& time) { return time.second; });
	auto first = warmed != myFrameTimes.end() ? warmed : myFrameTimes.begin() + static_cast<ptrdiff_t>(myFrameTimes.size() / 4);
	std::vector<float> settled;
	std::vector<float> warming;
	for (const auto& [time, warmingUp] : std::ranges::subrange(first, myFrameTimes.end()))
		(warmingUp ? warming : settled).push_back(time);
	auto median = [](std::vector<float>& times)
	{
		std::ranges::sort(times);
		return times[times.size() / 2];
	};
	if (!settled.empty())
		std::println("frame time: {:.3f} ms (median of {} half seconds)", median(settled), settled.size());
	if (!warming.empty())
		std::println("frame time warming up: {:.3f} ms (median of {} half seconds)", median(warming), warming.size());
}

} // namespace client::inline SPEEDO_GRAPHICS_API_NAMESPACE
