// the D3D12 backend's gpu profiler: its first timestamp, the queue's clock (the rest is every backend's,
// rhi/gpuprofiler.inl, over the query pools of querypool.cpp), and crash breadcrumbs: the passes as events on the command
// lists (PIX's and the debuggers' markers; the device removed's own report, DRED, comes with docs/backends-plan.md's
// step 6)
#include <rhi/d3d12/backend.h>

#include <rhi/crashreport.h>
#include <rhi/device.h>
#include <rhi/gpuprofiler.h>
#include <rhi/rhi.h>

#include <core/profiling.h>

#include <string>
#include <string_view>

namespace rhi
{

#if (SPEEDO_PROFILING_LEVEL > 0)

// the queue's clock now, in its timestamps' ticks (no command buffer needed)
template <>
uint64_t GpuProfiler<kD3D12>::InternalFirstTimestamp(QueueHandle<kD3D12> queue, CommandBufferHandle<kD3D12> /*setup*/)
{
	UINT64 gpu = 0;
	UINT64 cpu = 0;
	d3d12::Check(queue->GetClockCalibration(&gpu, &cpu), "ID3D12CommandQueue::GetClockCalibration");
	return gpu;
}

#endif

namespace gpuprofiler
{

// PIX's metadata of an event whose data is a null terminated string of chars (WINPIX_EVENT_ANSI_VERSION)
constexpr UINT kAnsiEvent = 1;

} // namespace gpuprofiler

// a pass's name as an event on the command list, which PIX and the debug layer's messages show
template <>
CrashBreadcrumb<kD3D12>::CrashBreadcrumb(CommandBufferHandle<kD3D12> cmd, std::string_view name)
	: myCmd(cmd)
{
	const std::string text(name);
	myCmd->list->BeginEvent(gpuprofiler::kAnsiEvent, text.c_str(), static_cast<UINT>(text.size() + 1));
	mySequence = 1;
}

template <>
void CrashBreadcrumb<kD3D12>::End()
{
	if (mySequence == 0)
		return;
	myCmd->list->EndEvent();
	mySequence = 0;
}

} // namespace rhi

#include <rhi/gpuprofiler.inl>

template class rhi::GpuProfiler<kD3D12>;
