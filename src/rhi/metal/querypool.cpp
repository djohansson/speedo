// the Metal backend's query pools come with the gpu profiler (counter sample buffers, docs/backends-plan.md, step 7):
// the device's profiler holds one, none made yet, so only its destructor is here
#include <rhi/metal/backend.h>

#include <rhi/querypool.h>
#include <rhi/rhi.h>

namespace rhi
{

template <>
QueryPool<kMetal>::~QueryPool()
{
	if (IsValid() && myPool != nullptr)
		myPool->release();
}

} // namespace rhi
