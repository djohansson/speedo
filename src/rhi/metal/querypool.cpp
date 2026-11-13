// the Metal backend's query pools of timestamps: counter sample buffers of the device's timestamp counter set, sampled
// where encoders begin and end (Apple's gpus sample nowhere else: MTLCounterSamplingPointAtStageBoundary). A timestamp
// written while recording is one of those samples (CommandBufferObject::Timestamp: the next encoder's start, or the end of
// the encoder open or just ended), read back once its command buffer has completed. The timestamps are nanoseconds, the
// cpu's clock (DeviceLimits::timestampPeriod 1). Acceleration structures' compacted sizes come with them (step 7)
#include <rhi/metal/backend.h>

#include <rhi/querypool.h>
#include <rhi/rhi.h>

#include <core/assert.h>

#include <algorithm>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace rhi
{

IMPLEMENT_OBJECT_GETINSTANCE(QueryPool<kMetal>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(QueryPool<kMetal>);

namespace metal
{

TimestampSamples QueryPoolObject::NewPair()
{
	// pair p is samples 2p and 2p + 1, never split between buffers (kSampleCount is even)
	const auto sample = (nextPair++ % queries.size()) * 2;
	return {.buffer = buffers[sample / kSampleCount], .start = static_cast<NS::UInteger>(sample % kSampleCount)};
}

} // namespace metal

namespace querypool
{

static void Release(metal::QueryPoolObject::Query& query) noexcept
{
	if (query.commandBuffer != nullptr)
		query.commandBuffer->release();
	query = {};
}

} // namespace querypool

template <>
QueryPool<kMetal>::QueryPool(CreateDescType&& desc)
	: DeviceObject<QueryPool<kMetal>>(std::forward<CreateDescType>(desc))
{
	const auto& poolDesc = GetDesc();
	ENSUREF(
		poolDesc.type == QueryType::kTimestamp,
		"the Metal backend's query pools of acceleration structures' compacted sizes come with them (docs/backends-plan.md, step 7)");
	ENSURE(poolDesc.count > 0);

	auto* device = static_cast<MTL::Device*>(GetDevice());
	MTL::CounterSet* timestamps = nullptr;
	NS::Array* sets = device->counterSets();
	for (NS::UInteger setIt = 0; sets != nullptr && setIt < sets->count(); setIt++)
		if (auto* set = sets->object<MTL::CounterSet>(setIt); set->name()->isEqualToString(MTL::CommonCounterSetTimestamp))
			timestamps = set;
	ENSUREF(timestamps != nullptr, "the Metal device has no timestamp counter set (Device reports no timestamp bits then)");

	auto* object = new metal::QueryPoolObject{};
	object->queries.resize(poolDesc.count);
	for (uint64_t samples = uint64_t{poolDesc.count} * 2; samples > 0;)
	{
		const auto count = std::min<uint64_t>(samples, metal::QueryPoolObject::kSampleCount);
		auto* descriptor = MTL::CounterSampleBufferDescriptor::alloc()->init();
		descriptor->setCounterSet(timestamps);
		descriptor->setStorageMode(MTL::StorageModeShared);
		descriptor->setSampleCount(count);
		descriptor->setLabel(NS::String::string(std::string(GetName()).c_str(), NS::UTF8StringEncoding));
		NS::Error* error = nullptr;
		auto* buffer = device->newCounterSampleBuffer(descriptor, &error);
		descriptor->release();
		ENSUREF(
			buffer != nullptr,
			"Failed to make a Metal counter sample buffer of {} samples: {}",
			count,
			error != nullptr ? error->localizedDescription()->utf8String() : "(no error)");
		object->buffers.push_back(buffer);
		samples -= count;
	}
	myPool = object;
}

template <>
void QueryPool<kMetal>::Swap(QueryPool& rhs) noexcept
{
	DeviceObject<QueryPool<kMetal>>::Swap(rhs);
	std::swap(myPool, rhs.myPool);
}

template <>
QueryPool<kMetal>::QueryPool(QueryPool&& other) noexcept
{
	Swap(other);
}

template <>
QueryPool<kMetal>::~QueryPool()
{
	if (!IsValid() || myPool == nullptr)
		return;
	for (auto& query : myPool->queries)
		querypool::Release(query);
	for (auto* buffer : myPool->buffers)
		buffer->release();
	delete myPool; //NOLINT(cppcoreguidelines-owning-memory)
}

template <>
QueryPool<kMetal>& QueryPool<kMetal>::operator=(QueryPool&& other) noexcept
{
	Swap(other);
	return *this;
}

template <>
void QueryPool<kMetal>::Reset(uint32_t first, uint32_t count) const
{
	std::scoped_lock lock{myPool->mutex};
	ENSURE(uint64_t{first} + count <= myPool->queries.size());
	for (uint32_t queryIt = first; queryIt < first + count; queryIt++)
		querypool::Release(myPool->queries[queryIt]);
}

template <>
void QueryPool<kMetal>::Reset(CommandBufferHandle<kMetal> /*cmd*/, uint32_t first, uint32_t count) const
{
	// the queries are the host's (a sample needs no reset before it is written again)
	Reset(first, count);
}

template <>
void QueryPool<kMetal>::WriteTimestamp(CommandBufferHandle<kMetal> cmd, uint32_t index, PipelineStage /*stage*/) const
{
	std::scoped_lock lock{myPool->mutex};
	auto& query = myPool->queries.at(index);
	querypool::Release(query);
	query.sample = cmd->Timestamp(*myPool);
	auto* commandBuffer = cmd->primary != nullptr ? cmd->primary->commandBuffer : cmd->commandBuffer;
	query.commandBuffer = commandBuffer->retain();
}

template <>
void QueryPool<kMetal>::Read(uint32_t first, std::span<std::optional<uint64_t>> timestamps) const
{
	std::scoped_lock lock{myPool->mutex};
	ENSURE(first + timestamps.size() <= myPool->queries.size());

	// those done (their command buffer completed, or failed), and per sample buffer the range of their samples
	struct Range
	{
		MTL::CounterSampleBuffer* buffer = nullptr;
		NS::UInteger first = ~NS::UInteger{0};
		NS::UInteger last = 0;
		const uint64_t* values = nullptr;
	};
	std::vector<Range> ranges;
	for (size_t queryIt = 0; queryIt < timestamps.size(); queryIt++)
	{
		const auto& query = myPool->queries[first + queryIt];
		timestamps[queryIt].reset();
		if (query.commandBuffer == nullptr || query.commandBuffer->status() < MTL::CommandBufferStatusCompleted)
			continue;
		timestamps[queryIt] = 0;
		if (!query.sample)
			continue;
		auto range = std::ranges::find(ranges, query.sample->buffer, &Range::buffer);
		if (range == ranges.end())
			range = ranges.insert(ranges.end(), Range{.buffer = query.sample->buffer});
		range->first = std::min(range->first, query.sample->index);
		range->last = std::max(range->last, query.sample->index);
	}
	if (ranges.empty())
		return;

	// resolved on the cpu (shared storage), a sample never written reading MTLCounterErrorValue: 0, as one not sampled
	NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
	for (auto& range : ranges)
	{
		NS::Data* data = range.buffer->resolveCounterRange(NS::Range::Make(range.first, range.last - range.first + 1));
		ENSUREF(data != nullptr, "Failed to resolve a Metal counter sample buffer's samples {} to {}", range.first, range.last);
		ENSURE(data->length() >= (range.last - range.first + 1) * sizeof(uint64_t));
		range.values = static_cast<const uint64_t*>(data->bytes());
	}
	for (size_t queryIt = 0; queryIt < timestamps.size(); queryIt++)
	{
		const auto& query = myPool->queries[first + queryIt];
		if (!timestamps[queryIt] || !query.sample)
			continue;
		const auto& range = *std::ranges::find(ranges, query.sample->buffer, &Range::buffer);
		const uint64_t value = range.values[query.sample->index - range.first];
		timestamps[queryIt] = value != MTL::CounterErrorValue ? value : 0;
	}
	pool->release();
}

} // namespace rhi
