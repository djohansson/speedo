// the Metal backend's query pools come with the gpu profiler (counter sample buffers, docs/backends-plan.md, step 7):
// the queue makes no profiler (the device reports no timestamp bits) and nothing else makes a pool without ray queries,
// so a pool made fails hard; the empty ones held are moved and destroyed here
#include <rhi/metal/backend.h>

#include <rhi/querypool.h>
#include <rhi/rhi.h>

#include <core/assert.h>

#include <optional>
#include <span>
#include <utility>

namespace rhi
{

IMPLEMENT_OBJECT_GETINSTANCE(QueryPool<kMetal>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(QueryPool<kMetal>);

namespace querypool
{

[[noreturn]] static void NotYet()
{
	ENSUREF(false, "the Metal backend's query pools come with its gpu profiler (docs/backends-plan.md, step 7)");
	__builtin_trap();
}

} // namespace querypool

template <>
QueryPool<kMetal>::QueryPool(CreateDescType&& desc)
	: DeviceObject<QueryPool<kMetal>>(std::forward<CreateDescType>(desc))
{
	querypool::NotYet();
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
	if (IsValid() && myPool != nullptr)
		myPool->release();
}

template <>
QueryPool<kMetal>& QueryPool<kMetal>::operator=(QueryPool&& other) noexcept
{
	Swap(other);
	return *this;
}

template <>
void QueryPool<kMetal>::Reset(CommandBufferHandle<kMetal> /*cmd*/, uint32_t /*first*/, uint32_t /*count*/) const
{
	querypool::NotYet();
}

template <>
void QueryPool<kMetal>::Reset(uint32_t /*first*/, uint32_t /*count*/) const
{
	querypool::NotYet();
}

template <>
void QueryPool<kMetal>::WriteTimestamp(CommandBufferHandle<kMetal> /*cmd*/, uint32_t /*index*/, PipelineStage /*stage*/) const
{
	querypool::NotYet();
}

template <>
void QueryPool<kMetal>::Read(uint32_t /*first*/, std::span<std::optional<uint64_t>> /*timestamps*/) const
{
	querypool::NotYet();
}

} // namespace rhi
