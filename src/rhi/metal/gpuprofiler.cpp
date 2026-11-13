// the Metal backend's gpu profiler and crash breadcrumbs: none yet (counter sample buffers, and the command buffers'
// error information, come with docs/backends-plan.md's step 7): scopes are empty, as Vulkan's backend's are without
// profiling. the queue makes no profiler (the device reports no timestamp bits), so only these are called
#include <rhi/metal/backend.h>

#include <rhi/crashreport.h>
#include <rhi/gpuprofiler.h>
#include <rhi/rhi.h>

#include <string_view>
#include <vector>

namespace rhi
{

template <>
GpuProfiler<kMetal>::Scope GpuProfiler<kMetal>::Begin(CommandBufferHandle<kMetal> /*cmd*/, const SourceLocationData& /*location*/, PipelineStage /*stage*/)
{
	return {};
}

template <>
GpuProfiler<kMetal>::Scope GpuProfiler<kMetal>::Begin(
	CommandBufferHandle<kMetal> /*cmd*/, std::string_view /*name*/, const SourceLocationData& /*location*/, uint64_t /*group*/, PipelineStage /*stage*/)
{
	return {};
}

template <>
void GpuProfiler<kMetal>::InternalEnd(CommandBufferHandle<kMetal> /*cmd*/, uint64_t /*begin*/, PipelineStage /*stage*/)
{}

template <>
void GpuProfiler<kMetal>::Collect()
{}

template <>
std::vector<GpuProfiler<kMetal>::Timing> GpuProfiler<kMetal>::TakeTimings(uint64_t /*group*/)
{
	return {};
}

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
