#pragma once

#include <rhi/types.h>

#include <core/std_extra.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace rhi
{

// crash reports: what a fatal graphics api error's (a lost device), or a hang's, report tells of what the application and
// the gpu were doing (see the backend's crashreport.cpp). the first thread to meet one writes the report, to
// <directory>/crash-<time>.json and a summary to stderr, and ends the process; the others wait for it. every build
// reports, release included

// where reports are written (the application's user profile's crashes directory): before the RHI is made
void SetCrashReportDirectory(std::string_view directory);

// something the application did that a report should tell (a swapchain recreated, a fullscreen switch, a model
// installed, ...), stamped with the frame and the time: the last few hundred are kept. any thread
void CrashEvent(std::string_view what);

// the frame being drawn, which events, submits and breadcrumbs are stamped with. the draw thread, as each frame starts
void CrashFrame(uint64_t frame);

// a section of the application's own in a report (what only it can tell of what the gpu was doing): write returns its
// json (a value: an object, an array, ...), which the report writes under name. it runs on the reporting thread, whichever
// that is, with the device lost or hung: it may read what the gpu wrote to host visible memory, but take no lock another
// thread may hold. set while what it reads lives; an empty write removes it
void SetCrashReportSection(std::string_view name, core::std_extra::move_only_function<std::string()> write);

// the draw thread's heartbeat: each time it is about to draw a frame, or skip one (minimized, a window change), and each
// time it has created a pipeline it bound (cold, a GI kernel takes seconds to compile on KosmicKrisp)
void CrashHeartbeat();
// the watchdog, from a thread that never waits for the draw thread (the main thread, several times a second): once there
// has been a heartbeat, one older than the hang timeout (SPEEDO_GPU_HANG_TIMEOUT, 20 s) is a hang, reported. it catches
// what the waits' own timeouts can't: the draw thread blocked inside a call without one (a present, a submit, waiting
// for the device to be idle)
void CrashWatch();

// a span in which the draw thread is busy with something long that makes progress where no heartbeat can tell (a
// pipeline created cold, inside the driver's call, or one it waits for a background job to create): the watch gives it
// 120 s from where the span began rather than the hang timeout from the last heartbeat. four clients compiling at once
// on KosmicKrisp, two waited past the timeout for their first frames' pipelines
class CrashBusy final
{
public:
	CrashBusy();
	~CrashBusy();

	CrashBusy(const CrashBusy&) = delete;
	CrashBusy& operator=(const CrashBusy&) = delete;
};

// a span of gpu work in a command buffer, by name (a frame graph pass): the gpu writes when it begins (at the top of the
// pipe) and ends (at the bottom), so that a report can tell what had begun and not ended when the device was lost or
// hung: with VK_AMD_buffer_marker (into a host visible ring), else VK_NV_device_diagnostic_checkpoints (the queues' last
// checkpoints, read back after the loss), else nothing. nothing either while the Crash Diagnostic Layer is loaded, whose
// own dump tells more (every command). its name is interned: pass the same names each frame
template <GraphicsApi G>
class CrashBreadcrumb final
{
public:
	CrashBreadcrumb(CommandBufferHandle<G> cmd, std::string_view name);
	~CrashBreadcrumb() { End(); }

	CrashBreadcrumb(const CrashBreadcrumb&) = delete;
	CrashBreadcrumb& operator=(const CrashBreadcrumb&) = delete;

	// its end, before it is dropped
	void End();

private:
	CommandBufferHandle<G> myCmd{};
	uint64_t mySequence = 0; // 0: none recorded (or ended)
};

} // namespace rhi
