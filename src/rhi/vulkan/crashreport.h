#pragma once

#include <rhi/types.h>

#include <cstdint>
#include <format>
#include <string_view>
#include <vector>

// crash reports, the backend's side (see rhi/crashreport.h): what the device can tell after a lost device or a hang, the
// gpu breadcrumbs, the logs of submits and events, and the report. one device reports: the primary one (the RHI makes
// no other)

namespace rhi
{
template <GraphicsApi G>
class Device;
}

namespace rhi::vk::crash
{

// what the device can tell, decided before it is made (Detect)
struct Capabilities
{
	// the Crash Diagnostic Layer is loaded (it names itself through VK_EXT_tooling_info): its dump tells every command in
	// flight, so no breadcrumbs are recorded (and the queues' checkpoints, its own, aren't read)
	bool crashDiagnosticLayer = false;
	bool bufferMarker = false; // VK_AMD_buffer_marker: the breadcrumbs' begin and end written into a ring
	bool checkpoints = false; // VK_NV_device_diagnostic_checkpoints: where buffer markers aren't
	bool deviceFault = false; // VK_EXT_device_fault: the faulting address and the vendor's codes after a loss
	bool deviceFaultVendorBinary = false; // its vendor binary, saved beside the report
	// VK_EXT_device_address_binding_report: which object a faulting address was bound to (through VK_EXT_debug_utils'
	// messenger, see CreateMessenger)
	bool addressBindingReport = false;
	bool deviceCoherentMemory = false; // VK_AMD_device_coherent_memory: the breadcrumbs' ring in device coherent memory
};

// whether the instance enables VK_EXT_debug_utils for the address binding reports (in every build where it exists)
[[nodiscard]] bool WantsDebugUtils(VkInstance instance);
// the instance's messenger of address bindings (where the instance has VK_EXT_debug_utils), made after the instance and
// destroyed before it
void CreateMessenger(VkInstance instance, const VkAllocationCallbacks* allocator);
void DestroyMessenger(VkInstance instance, const VkAllocationCallbacks* allocator);

// before the device is made: what it can do (from its extensions, and the tools loaded), and the extensions it enables
// for it. its features come with the device's others (GetPhysicalDeviceInfo2 lists them where their extensions are)
void Detect(VkInstance instance, VkPhysicalDevice physicalDevice);
[[nodiscard]] const Capabilities& GetCapabilities() noexcept;
void AddDeviceExtensions(std::vector<const char*>& extensions, VkInstance instance, VkPhysicalDevice physicalDevice);
// whether the device's feature chain (GetPhysicalDeviceInfo2) holds an extension's features, the crash reports' (device
// fault, address binding report, device coherent memory): where it has the extension (and what it needs), so that
// AddDeviceExtensions enables it whenever its features are there
[[nodiscard]] bool ChainsFeatures(const char* extension, VkInstance instance, VkPhysicalDevice physicalDevice);
// once the device is made (the breadcrumbs' ring), and before its allocator is destroyed
void Init(Device<kVk>& device);
void Shutdown();

// a submit, for the report's log of them: the queue, its command buffers, and the timeline value it signals (0: none)
void OnSubmit(VkQueue queue, uint32_t commandBufferCount, uint64_t timelineValue);

// a fatal result (a lost device, out of memory, any other error): writes the report and ends the process; a thread
// meeting one while another reports waits for it. queue: the queue the call was on, if any
[[noreturn]] void ReportFatal(VkResult result, VkQueue queue);
// a wait without a timeout that took longer than HangTimeout: the same report, of a hang (the device is still alive, so
// its breadcrumbs and checkpoints are current)
[[noreturn]] void ReportHang(std::string_view what);
// how long a wait without a timeout may take before it is a hang (SPEEDO_GPU_HANG_TIMEOUT, in seconds, 0 for never;
// 20 by default), in nanoseconds: ~0 for never
[[nodiscard]] uint64_t HangTimeout() noexcept;

// a wait: one without a timeout (~0) waits for HangTimeout at most, and reports a hang if it passes. wait: the call,
// taking the timeout to wait for; what: what waited, for the report (called only then)
template <typename WaitFn, typename WhatFn>
VkResult WatchedWait(uint64_t timeout, WhatFn&& what, WaitFn&& wait)
{
	auto hangTimeout = HangTimeout();
	if (timeout != ~0ULL || hangTimeout == ~0ULL)
		return wait(timeout);
	auto result = wait(hangTimeout);
	if (result == VK_TIMEOUT)
		ReportHang(std::format("{} waited longer than {} s", what(), hangTimeout / 1'000'000'000ULL)); //NOLINT(readability-magic-numbers)
	return result;
}

} // namespace rhi::vk::crash
