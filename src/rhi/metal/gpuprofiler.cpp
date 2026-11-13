// the Metal backend's gpu profiler: its first timestamp, the device's (the rest is every backend's,
// rhi/gpuprofiler.inl; the timestamps are counter samples where encoders begin and end, see querypool.cpp), and crash
// breadcrumbs: none yet (the command buffers' errors are reported as they complete, see command.cpp)
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

template <>
CrashBreadcrumb<kMetal>::CrashBreadcrumb(CommandBufferHandle<kMetal> cmd, std::string_view /*name*/)
	: myCmd(cmd)
{}

template <>
void CrashBreadcrumb<kMetal>::End()
{
	myCmd = nullptr;
}

} // namespace rhi

#include <rhi/gpuprofiler.inl>

template class rhi::GpuProfiler<kMetal>;
