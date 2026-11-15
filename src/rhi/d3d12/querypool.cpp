// the D3D12 backend's query pools: query heaps resolved to a buffer (docs/backends-plan.md, step 6: timestamps and
// acceleration structures' compacted sizes). until then none is made (the queues take no timestamps, see device.cpp), and
// its destructor, which the queues' gpu profilers reference, has nothing to release
#include <rhi/d3d12/backend.h>

#include <rhi/querypool.h>

#include <core/assert.h>

namespace rhi
{

template <>
QueryPool<kD3D12>::~QueryPool()
{
	ENSUREF(!IsValid() || myPool == nullptr, "the D3D12 backend has no query pools yet (docs/backends-plan.md, step 6)");
}

} // namespace rhi
