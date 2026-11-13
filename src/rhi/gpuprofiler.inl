// the gpu profiler's members, every backend's: included by each backend's gpuprofiler.cpp after its own
// InternalFirstTimestamp (the context's first timestamp, which Tracy aligns with the cpu's time), and instantiated there
// (Tracy's C api, which these call, is declared with TRACY_ENABLE alone: see core/profiling.h)
#include <core/assert.h>
#include <core/profiling.h>

#if (SPEEDO_PROFILING_LEVEL > 0)

#include <tracy/TracyC.h>

#include <algorithm>
#include <functional>
#include <optional>
#include <span>
#include <utility>

namespace rhi
{

namespace gpuprofiler
{

// a scope longer than this is a timestamp the backend didn't write (see Collect)
constexpr double kMaxMilliseconds = 250.0;
// at most this many timings kept per group no one takes
constexpr size_t kMaxGroupTimings = 4096;
// Tracy's gpu context types (tracy::GpuContextType): its Vulkan's and its Metal's
template <GraphicsApi G>
constexpr uint8_t kTracyContextType = G == kMetal ? 6 : 2;

} // namespace gpuprofiler

template <GraphicsApi G>
GpuProfiler<G>::GpuProfiler(QueryPool<G>&& pool, QueueHandle<G> queue, CommandBufferHandle<G> setup, std::string_view name)
	: myPool(std::forward<QueryPool<G>>(pool))
	, myQueries(myPool.GetDesc().count)
	, myPeriod(myPool.GetDevice().GetLimits().timestampPeriod)
{
	ENSURE(!myQueries.empty() && myQueries.size() <= 65536); // Tracy's query ids are 16 bit

	myPool.Reset(0, static_cast<uint32_t>(myQueries.size()));

	auto gpuTime = static_cast<int64_t>(InternalFirstTimestamp(queue, setup));
	myLastTime = static_cast<uint64_t>(gpuTime);

	myContext = tracy::GetGpuCtxCounter().fetch_add(1, std::memory_order_relaxed);
	ENSURE(myContext != 255);
	___tracy_emit_gpu_new_context_serial({
		.gpuTime = gpuTime,
		.period = static_cast<float>(myPeriod),
		.context = myContext,
		.flags = 0,
		.type = gpuprofiler::kTracyContextType<G>,
	});
	___tracy_emit_gpu_context_name_serial({.context = myContext, .name = name.data(), .len = static_cast<uint16_t>(name.size())});
}

template <GraphicsApi G>
uint64_t GpuProfiler<G>::InternalWrite(CommandBufferHandle<G> cmd, PipelineStage stage)
{
	// called with myMutex held. the ring fills if Collect stops freeing it: a command buffer recorded with scopes and never
	// submitted keeps its queries from ever being done
	ENSUREF(myHead - myTail < myQueries.size(), "the gpu profiler's {} timestamps are all in use", myQueries.size());
	auto index = myHead++;
	auto& query = myQueries[index % myQueries.size()];
	query = {.cmd = cmd};
	myPool.WriteTimestamp(cmd, static_cast<uint32_t>(index % myQueries.size()), stage);
	return index;
}

template <GraphicsApi G>
typename GpuProfiler<G>::Scope GpuProfiler<G>::Begin(CommandBufferHandle<G> cmd, const SourceLocationData& location, PipelineStage stage)
{
	static_assert(sizeof(SourceLocationData) == sizeof(___tracy_source_location_data));
	static_assert(offsetof(SourceLocationData, name) == offsetof(___tracy_source_location_data, name));
	static_assert(offsetof(SourceLocationData, function) == offsetof(___tracy_source_location_data, function));
	static_assert(offsetof(SourceLocationData, file) == offsetof(___tracy_source_location_data, file));
	static_assert(offsetof(SourceLocationData, line) == offsetof(___tracy_source_location_data, line));
	static_assert(offsetof(SourceLocationData, color) == offsetof(___tracy_source_location_data, color));

	std::scoped_lock lock(myMutex);
	auto index = InternalWrite(cmd, stage);
	___tracy_emit_gpu_zone_begin_serial({
		.srcloc = reinterpret_cast<uint64_t>(&location),
		.queryId = static_cast<uint16_t>(index % myQueries.size()),
		.context = myContext});
	return {this, cmd, index};
}

template <GraphicsApi G>
typename GpuProfiler<G>::Scope GpuProfiler<G>::Begin(
	CommandBufferHandle<G> cmd, std::string_view name, const SourceLocationData& location, uint64_t group, PipelineStage stage)
{
	auto color = location.color != 0 ? location.color
									 : 0xFF000000U | (static_cast<uint32_t>(std::hash<std::string_view>{}(name)) & 0x00FFFFFFU);
	auto srcloc = ___tracy_alloc_srcloc_name(
		location.line,
		location.file,
		std::char_traits<char>::length(location.file),
		location.function,
		std::char_traits<char>::length(location.function),
		name.data(),
		name.size(),
		color);

	std::scoped_lock lock(myMutex);
	auto index = InternalWrite(cmd, stage);
	auto& query = myQueries[index % myQueries.size()];
	query.group = group;
	if (group != 0)
		query.name = name;
	___tracy_emit_gpu_zone_begin_alloc_serial(
		{.srcloc = srcloc, .queryId = static_cast<uint16_t>(index % myQueries.size()), .context = myContext});
	return {this, cmd, index};
}

template <GraphicsApi G>
void GpuProfiler<G>::InternalEnd(CommandBufferHandle<G> cmd, uint64_t begin, PipelineStage stage)
{
	std::scoped_lock lock(myMutex);
	auto index = InternalWrite(cmd, stage);
	if (begin >= myTail)
		myQueries[begin % myQueries.size()].end = index;
	___tracy_emit_gpu_zone_end_serial({.queryId = static_cast<uint16_t>(index % myQueries.size()), .context = myContext});
}

template <GraphicsApi G>
void GpuProfiler<G>::Collect()
{
	ZoneScopedN("GpuProfiler::Collect");

	std::scoped_lock lock(myMutex);

	auto size = myQueries.size();
	auto count = myHead - myTail;
	if (count == 0)
		return;

	// the queries from the tail, in two reads where the ring wraps
	std::vector<std::optional<uint64_t>> times(count);
	auto first = myTail % size;
	auto firstCount = std::min<uint64_t>(count, size - first);
	myPool.Read(static_cast<uint32_t>(first), std::span(times.data(), firstCount));
	if (firstCount < count)
		myPool.Read(0, std::span(times.data() + firstCount, count - firstCount));

	// those the gpu is done with: up to the first it isn't, and not past a scope whose end isn't (an open one's, or one
	// recorded later into a command buffer still running), so that a scope's two are collected together
	uint64_t done = 0;
	while (done < count && times[done])
		done++;
	for (uint64_t queryIt = 0; queryIt < done; queryIt++)
	{
		const auto& query = myQueries[(myTail + queryIt) % size];
		if (query.group != 0 && (query.end == 0 || query.end >= myTail + done))
		{
			done = queryIt;
			break;
		}
	}
	if (done == 0)
		return;

	// which are plausible: some backends don't write every timestamp, leaving one written frames before, and report it
	// available anyway (KosmicKrisp only writes them where its own Metal command encoders begin and end, and those of a
	// compute encoder that holds dispatches read 0, or one a second old; the Metal backend samples where its encoders
	// begin and end, see its QueryPool). a command buffer's timestamps are written in the order they were recorded, so
	// each must be non-zero, at least the one before it in its command buffer, and not more than a quarter of a second
	// after it. Tracy gets the one before in place of the others
	auto maxTicks = static_cast<uint64_t>(gpuprofiler::kMaxMilliseconds * 1e6 / myPeriod);
	std::vector<std::pair<CommandBufferHandle<G>, uint64_t>> floors; // per command buffer, its last plausible one
	std::vector<bool> plausible(done);
	for (uint64_t queryIt = 0; queryIt < done; queryIt++)
	{
		auto index = myTail + queryIt;
		const auto& query = myQueries[index % size];
		auto floor = std::ranges::find(floors, query.cmd, &std::pair<CommandBufferHandle<G>, uint64_t>::first);
		if (floor == floors.end())
			floor = floors.insert(floors.end(), {query.cmd, 0});
		auto time = *times[queryIt];
		plausible[queryIt] = time != 0 && time >= floor->second && (floor->second == 0 || time - floor->second <= maxTicks);
		if (plausible[queryIt])
		{
			floor->second = time;
			myLastTime = std::max(myLastTime, time);
		}
		else
			time = floor->second != 0 ? floor->second : myLastTime;
		times[queryIt] = time;
		___tracy_emit_gpu_time_serial(
			{.gpuTime = static_cast<int64_t>(time), .queryId = static_cast<uint16_t>(index % size), .context = myContext});
	}

	// the groups' scopes whose two timestamps are plausible
	for (uint64_t queryIt = 0; queryIt < done; queryIt++)
	{
		auto& query = myQueries[(myTail + queryIt) % size];
		if (query.group == 0)
			continue;
		auto end = query.end - myTail;
		if (plausible[queryIt] && plausible[end])
		{
			auto milliseconds = static_cast<double>(*times[end] - *times[queryIt]) * myPeriod * 1e-6;
			auto& timings = myTimings[query.group];
			if (timings.size() >= gpuprofiler::kMaxGroupTimings)
				timings.erase(timings.begin(), timings.begin() + static_cast<ptrdiff_t>(timings.size() / 2));
			timings.push_back({.name = std::move(query.name), .milliseconds = milliseconds});
		}
	}

	// free them, from the host: they are done
	for (uint64_t queryIt = 0; queryIt < done;)
	{
		auto at = (myTail + queryIt) % size;
		auto run = std::min<uint64_t>(done - queryIt, size - at);
		myPool.Reset(static_cast<uint32_t>(at), static_cast<uint32_t>(run));
		queryIt += run;
	}
	myTail += done;
}

template <GraphicsApi G>
std::vector<typename GpuProfiler<G>::Timing> GpuProfiler<G>::TakeTimings(uint64_t group)
{
	std::scoped_lock lock(myMutex);
	auto it = myTimings.find(group);
	if (it == myTimings.end())
		return {};
	return std::exchange(it->second, {});
}

} // namespace rhi

#else

// builds that don't profile make no profilers (see Queue): what calls them links, and does nothing

namespace rhi
{

template <GraphicsApi G>
typename GpuProfiler<G>::Scope GpuProfiler<G>::Begin(CommandBufferHandle<G>, const SourceLocationData&, PipelineStage)
{
	return {};
}

template <GraphicsApi G>
typename GpuProfiler<G>::Scope GpuProfiler<G>::Begin(CommandBufferHandle<G>, std::string_view, const SourceLocationData&, uint64_t, PipelineStage)
{
	return {};
}

template <GraphicsApi G>
void GpuProfiler<G>::InternalEnd(CommandBufferHandle<G>, uint64_t, PipelineStage)
{}

template <GraphicsApi G>
void GpuProfiler<G>::Collect()
{}

template <GraphicsApi G>
std::vector<typename GpuProfiler<G>::Timing> GpuProfiler<G>::TakeTimings(uint64_t)
{
	return {};
}

} // namespace rhi

#endif // SPEEDO_PROFILING_LEVEL > 0
