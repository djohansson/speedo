// the Metal backend's gpu profiler: its first timestamp, the device's (the rest is every backend's,
// rhi/gpuprofiler.inl; the timestamps are counter samples where encoders begin and end, see querypool.cpp), and crash
// breadcrumbs: the passes' names on the encoders, which a failed command buffer's report prints (see command.cpp)
#include <rhi/metal/backend.h>

#include <rhi/crashreport.h>
#include <rhi/device.h>
#include <rhi/gpuprofiler.h>
#include <rhi/rhi.h>

#include <core/profiling.h>

#include <string_view>

namespace rhi
{

#if (SPEEDO_PROFILING_LEVEL > 0)

// the gpu's clock now, in the counters' units (nanoseconds, as the cpu's: no command buffer needed)
template <>
uint64_t GpuProfiler<kMetal>::InternalFirstTimestamp(QueueHandle<kMetal> /*queue*/, CommandBufferHandle<kMetal> /*setup*/)
{
	MTL::Timestamp cpu = 0;
	MTL::Timestamp gpu = 0;
	static_cast<MTL::Device*>(myPool.GetDevice())->sampleTimestamps(&cpu, &gpu);
	return gpu;
}

#endif

// a pass's name on the command buffer's encoders, which a failed command buffer's report prints (an encoder's label and
// debug signposts, see CommandBufferObject::BeginBreadcrumb); no report of the device's own yet (docs/backends-plan.md,
// step 10)
template <>
CrashBreadcrumb<kMetal>::CrashBreadcrumb(CommandBufferHandle<kMetal> cmd, std::string_view name)
	: myCmd(cmd)
{
	myCmd->BeginBreadcrumb(name);
	mySequence = 1;
}

template <>
void CrashBreadcrumb<kMetal>::End()
{
	if (mySequence == 0)
		return;
	myCmd->EndBreadcrumb();
	mySequence = 0;
}

} // namespace rhi

#include <rhi/gpuprofiler.inl>

template class rhi::GpuProfiler<kMetal>;
