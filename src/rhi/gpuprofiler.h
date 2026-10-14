#pragma once

#include <rhi/capi.h>
#include <rhi/enums.h>
#include <rhi/querypool.h>
#include <rhi/types.h>

#include <core/utils.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace rhi
{

// a queue's gpu scopes: timestamps written around commands, in a ring of queries, read back once the gpu is done with
// them (Collect) and handed to both Tracy (gpu zones of a context of the queue's, through its C api) and the
// application (each grouped scope's duration, TakeTimings). the one path gpu work is timed by: GPU_SCOPE and the frame
// graph's passes. only in builds that profile (SPEEDO_PROFILING_LEVEL > 0), see Queue::GetProfiler
template <GraphicsApi G>
class GpuProfiler final
{
public:
	// a scope's end: written when it is dropped (at the bottom of the pipe), or by End with another stage
	class Scope final
	{
	public:
		constexpr Scope() noexcept = default;
		Scope(Scope&& other) noexcept { Swap(other); }
		~Scope() { End(); }

		Scope& operator=(Scope&& other) noexcept
		{
			End();
			Swap(other);
			return *this;
		}

		void End(PipelineStage stage = PipelineStage::kBottomOfPipe)
		{
			if (myProfiler != nullptr)
				myProfiler->InternalEnd(myCmd, myBegin, stage);
			myProfiler = nullptr;
		}

		// made by GpuProfiler::Begin
		Scope(GpuProfiler* profiler, CommandBufferHandle<G> cmd, uint64_t begin) noexcept
			: myProfiler(profiler)
			, myCmd(cmd)
			, myBegin(begin)
		{}

	private:
		void Swap(Scope& other) noexcept
		{
			std::swap(myProfiler, other.myProfiler);
			std::swap(myCmd, other.myCmd);
			std::swap(myBegin, other.myBegin);
		}

		GpuProfiler* myProfiler = nullptr;
		CommandBufferHandle<G> myCmd{};
		uint64_t myBegin = 0;
	};

	// setup: a command buffer of the queue's, not begun, for the context's first timestamp (recorded, submitted and waited
	// for here, with the queue otherwise idle)
	GpuProfiler(QueryPool<G>&& pool, QueueHandle<G> queue, CommandBufferHandle<G> setup, std::string_view name);
	GpuProfiler(const GpuProfiler&) = delete;
	GpuProfiler& operator=(const GpuProfiler&) = delete;

	// begins a scope in cmd: a timestamp once the commands before reach stage. location names it in Tracy (its name,
	// static), or name does (any string: the frame graph's passes). a scope of a group (non-zero) is also timed for the
	// application (TakeTimings). inside a render pass with inline contents, top of pipe to bottom of pipe times the pass's
	// own work (see FrameGraph::Execute)
	[[nodiscard]] Scope Begin(
		CommandBufferHandle<G> cmd, const SourceLocationData& location, PipelineStage stage = PipelineStage::kBottomOfPipe);
	[[nodiscard]] Scope Begin(
		CommandBufferHandle<G> cmd,
		std::string_view name,
		const SourceLocationData& location,
		uint64_t group,
		PipelineStage stage = PipelineStage::kBottomOfPipe);

	// reads back the timestamps the gpu is done with (in the order they were recorded, up to the first it isn't done
	// with), hands them to Tracy and the groups' timings, and frees their queries. any thread, without the queue's lock
	void Collect();

	// the durations of a group's scopes collected since the last call, in the order they were recorded (milliseconds).
	// only the plausible ones: some backends don't write every timestamp (see Collect)
	struct Timing
	{
		std::string name;
		double milliseconds = 0.0;
	};
	[[nodiscard]] std::vector<Timing> TakeTimings(uint64_t group);

	// a group for TakeTimings, unique to the process
	[[nodiscard]] static uint64_t NewGroup() noexcept
	{
		static std::atomic_uint64_t next = 1;
		return next.fetch_add(1, std::memory_order_relaxed);
	}

private:
	struct Query
	{
		CommandBufferHandle<G> cmd{};
		uint64_t end = 0; // a scope's begin: its end's query, or 0 while it is open (a scope's end: 0)
		uint64_t group = 0; // a scope's begin: its group
		std::string name; // a scope's begin of a group: its name
	};

	[[nodiscard]] uint64_t InternalWrite(CommandBufferHandle<G> cmd, PipelineStage stage);
	void InternalEnd(CommandBufferHandle<G> cmd, uint64_t begin, PipelineStage stage);

	QueryPool<G> myPool;
	std::mutex myMutex;
	std::vector<Query> myQueries; // by query index
	uint64_t myHead = 1; // the next query to write (counting from 1: 0 is an open scope's end), its index modulo the count
	uint64_t myTail = 1; // the first not collected
	uint64_t myLastTime = 0; // the last plausible timestamp
	double myPeriod = 0.0; // nanoseconds per tick
	uint8_t myContext = 0; // Tracy's
	core::UnorderedMap<uint64_t, std::vector<Timing>> myTimings;
};

} // namespace rhi
