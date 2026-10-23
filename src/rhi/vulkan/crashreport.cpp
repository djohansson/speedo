#include "crashreport.h"
#include "utils.h"

#include <rhi/crashreport.h>
#include <rhi/device.h>
#include <rhi/instance.h>
#include <rhi/rhi.h>

#include <core/assert.h>
#include <core/file.h>
#include <core/utils.h>

#include <vulkan/vk_enum_string_helper.h>
#include <xxhash.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <mutex>
#include <print>
#include <string>
#include <thread>
#include <vector>

namespace rhi::vk::crash
{

namespace detail
{

// the breadcrumbs' ring: a frame's passes (about 40) times the frames in flight, many times over, so that a slot is
// never reused while the gpu may still write it
constexpr uint32_t kSlotCount = 4096;
constexpr uint32_t kEventCount = 256;
constexpr uint32_t kSubmitCount = 64;
constexpr uint32_t kQueueCount = 64;
// the breadcrumbs a report lists: the frames before the last one begun
constexpr uint64_t kReportFrameCount = 4;
constexpr const char* kCrashDiagnosticLayerName = "VK_LAYER_LUNARG_crash_diagnostic";

const auto kStart = std::chrono::steady_clock::now();

double Seconds() noexcept
{
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - kStart).count();
}

Capabilities gCapabilities;
std::string gDirectory;
std::atomic_uint64_t gFrame{0};
std::atomic_int64_t gHeartbeat{0}; // nanoseconds since kStart of the draw thread's last heartbeat (0: none yet)
std::atomic_bool gReporting{false};

// the log of events
struct Event
{
	double seconds = 0.0;
	uint64_t frame = 0;
	std::string what;
};
std::mutex gEventMutex;
std::array<Event, kEventCount> gEvents;
uint64_t gEventTotal = 0;

// the log of submits
struct Submit
{
	double seconds = 0.0;
	uint64_t frame = 0;
	VkQueue queue = VK_NULL_HANDLE;
	uint32_t commandBufferCount = 0;
	uint64_t timelineValue = 0;
};
std::mutex gSubmitMutex;
std::array<Submit, kSubmitCount> gSubmits;
uint64_t gSubmitTotal = 0;

// the queues submitted to, whose checkpoints a report reads (no lock: a report mustn't wait for one a lost device's
// thread holds)
std::array<std::atomic<VkQueue>, kQueueCount> gQueues{};

// the breadcrumbs' names, interned
std::mutex gNameMutex;
core::UnorderedMap<std::string, uint32_t> gNameIds;
std::vector<std::string> gNames;

uint32_t InternName(std::string_view name)
{
	std::scoped_lock lock(gNameMutex);
	auto [nameIt, inserted] = gNameIds.try_emplace(std::string(name), static_cast<uint32_t>(gNames.size()));
	if (inserted)
		gNames.emplace_back(name);
	return nameIt->second;
}

std::string NameOf(uint32_t nameId)
{
	std::scoped_lock lock(gNameMutex);
	return nameId < gNames.size() ? gNames[nameId] : std::string("?");
}

// the breadcrumbs: per slot, the cpu's record of it, and (with buffer markers) the gpu's begin and end, the low 32 bits
// of its sequence number, in a host visible ring
struct Slot
{
	std::atomic_uint64_t sequence{0}; // 0: never used
	uint32_t name = 0;
	uint64_t frame = 0;
};
std::array<Slot, kSlotCount> gSlots;
std::atomic_uint64_t gNextSequence{1};

VkDevice gDevice = VK_NULL_HANDLE;
VmaAllocator gAllocator = VK_NULL_HANDLE;
VkBuffer gMarkerBuffer = VK_NULL_HANDLE;
VmaAllocation gMarkerMemory = VK_NULL_HANDLE;
volatile uint32_t* gMarkers = nullptr;
PFN_vkCmdWriteBufferMarkerAMD gWriteBufferMarker = nullptr;
PFN_vkGetDeviceFaultInfoEXT gGetDeviceFaultInfo = nullptr;

// the device, as a report names it
struct DeviceDescription
{
	std::string name;
	std::string driverName;
	std::string driverInfo;
	uint32_t vendorId = 0;
	uint32_t deviceId = 0;
	uint32_t driverVersion = 0;
	uint32_t apiVersion = 0;
	std::vector<std::string> tools;
};
DeviceDescription gDescription;

// the objects bound to device addresses (VK_EXT_device_address_binding_report), by base address
struct Binding
{
	uint64_t base = 0;
	uint64_t size = 0;
	VkObjectType type = VK_OBJECT_TYPE_UNKNOWN;
	uint64_t handle = 0;
	std::string name;
	double unboundSeconds = -1.0; // when it was unbound (Seconds), or -1 while bound
};
// the last ranges unbound, kept: a fault at an address of a destroyed object (one written after it was freed) names it
constexpr uint32_t kUnboundCount = 1024;
std::mutex gBindingMutex;
core::UnorderedMap<uint64_t, Binding> gBindings;
std::array<Binding, kUnboundCount> gUnbound;
uint64_t gUnboundTotal = 0;
VkDebugUtilsMessengerEXT gMessenger = VK_NULL_HANDLE;

VKAPI_ATTR VkBool32 VKAPI_CALL OnAddressBinding(
	VkDebugUtilsMessageSeverityFlagBitsEXT /*severity*/,
	VkDebugUtilsMessageTypeFlagsEXT types,
	const VkDebugUtilsMessengerCallbackDataEXT* data,
	void* /*userData*/)
{
	if ((types & VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT) == 0U || data == nullptr)
		return VK_FALSE;

	const auto* next = static_cast<const VkBaseInStructure*>(data->pNext);
	while (next != nullptr && next->sType != VK_STRUCTURE_TYPE_DEVICE_ADDRESS_BINDING_CALLBACK_DATA_EXT)
		next = next->pNext;
	if (next == nullptr)
		return VK_FALSE;

	const auto& binding = *reinterpret_cast<const VkDeviceAddressBindingCallbackDataEXT*>(next);
	std::scoped_lock lock(gBindingMutex);
	if (binding.bindingType == VK_DEVICE_ADDRESS_BINDING_TYPE_UNBIND_EXT)
	{
		if (auto bound = gBindings.find(binding.baseAddress); bound != gBindings.end())
		{
			bound->second.unboundSeconds = Seconds();
			gUnbound[gUnboundTotal++ % kUnboundCount] = std::move(bound->second);
			gBindings.erase(bound);
		}
		return VK_FALSE;
	}
	Binding entry{.base = binding.baseAddress, .size = binding.size};
	if (data->objectCount > 0)
	{
		entry.type = data->pObjects[0].objectType;
		entry.handle = data->pObjects[0].objectHandle;
		if (data->pObjects[0].pObjectName != nullptr)
			entry.name = data->pObjects[0].pObjectName;
	}
	gBindings.insert_or_assign(binding.baseAddress, std::move(entry));
	return VK_FALSE;
}

bool HasInstanceExtension(const char* name)
{
	uint32_t count = 0;
	vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
	std::vector<VkExtensionProperties> extensions(count);
	vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data());
	return std::ranges::any_of(extensions, [name](const auto& extension) { return std::strcmp(extension.extensionName, name) == 0; });
}

// a string as a json string's contents
std::string Escape(std::string_view text)
{
	std::string result;
	result.reserve(text.size());
	for (char character : text)
	{
		switch (character)
		{
		case '"': result += "\\\""; break;
		case '\\': result += "\\\\"; break;
		case '\n': result += "\\n"; break;
		case '\r': result += "\\r"; break;
		case '\t': result += "\\t"; break;
		default:
			if (constexpr unsigned char kFirstPrintable = 0x20; static_cast<unsigned char>(character) < kFirstPrintable)
				result += std::format("\\u{:04x}", static_cast<unsigned>(character));
			else
				result += character;
		}
	}
	return result;
}

constexpr uint32_t kNvidiaVendorId = 0x10DE;
constexpr uint32_t kIntelVendorId = 0x8086;
constexpr uint64_t kNanosecondsPerSecond = 1'000'000'000ULL;

// a driver's version as its vendor numbers it (NVIDIA's 10, 8, 8 and 6 bits; Intel's on Windows 18 and 14)
std::string DriverVersionString(uint32_t vendorId, uint32_t version)
{
	if (vendorId == kNvidiaVendorId)
		return std::format("{}.{}.{}.{}", version >> 22U, (version >> 14U) & 0xFFU, (version >> 6U) & 0xFFU, version & 0x3FU); //NOLINT(readability-magic-numbers)
#ifdef _WIN32
	if (vendorId == kIntelVendorId)
		return std::format("{}.{}", version >> 14U, version & 0x3FFFU); //NOLINT(readability-magic-numbers)
#endif
	return std::format("{}.{}.{}", VK_API_VERSION_MAJOR(version), VK_API_VERSION_MINOR(version), VK_API_VERSION_PATCH(version));
}

constexpr const char* kBuildConfig =
#if (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
	"debug";
#elif (SPEEDO_PROFILING_LEVEL > 0)
	"profile";
#else
	"release";
#endif

constexpr const char* kPlatform =
#ifdef _WIN32
	"windows";
#elifdef __APPLE__
	"macos";
#elifdef __linux__
	"linux";
#else
	"unknown";
#endif

// a breadcrumb, as a report tells it
struct Crumb
{
	uint64_t sequence = 0;
	uint64_t frame = 0;
	std::string name;
	bool began = false;
	bool ended = false;
};

// the breadcrumbs of the last frames, by sequence, with what the gpu wrote of them (buffer markers)
std::vector<Crumb> ReadBufferMarkers()
{
	std::vector<Crumb> crumbs;
	if (gMarkers == nullptr)
		return crumbs;

	auto frame = gFrame.load(std::memory_order_relaxed);
	for (uint32_t slotIt = 0; slotIt < kSlotCount; slotIt++)
	{
		const auto& slot = gSlots[slotIt];
		auto sequence = slot.sequence.load(std::memory_order_acquire);
		if (sequence == 0 || slot.frame + kReportFrameCount < frame)
			continue;
		auto value = static_cast<uint32_t>(sequence);
		crumbs.push_back(Crumb{
			.sequence = sequence,
			.frame = slot.frame,
			.name = NameOf(slot.name),
			.began = gMarkers[static_cast<size_t>(slotIt) * 2] == value,
			.ended = gMarkers[(static_cast<size_t>(slotIt) * 2) + 1] == value});
	}
	std::ranges::sort(crumbs, {}, &Crumb::sequence);
	return crumbs;
}

// a checkpoint's marker: a breadcrumb's sequence number and whether it is its end
void* EncodeCheckpoint(uint64_t sequence, bool end) noexcept
{
	return reinterpret_cast<void*>(static_cast<uintptr_t>((sequence << 1U) | (end ? 1U : 0U))); //NOLINT(performance-no-int-to-ptr)
}

// the last checkpoints each queue submitted to reached (NV checkpoints), as json, and the breadcrumbs in flight: those
// whose begin is the last a queue's bottom of the pipe reached
std::string ReadCheckpoints(std::vector<Crumb>& inFlight)
{
	std::string json;
	if (!gCapabilities.checkpoints || gVkGetQueueCheckpointData2NV == nullptr)
		return json;

	for (const auto& entry : gQueues)
	{
		auto* queue = entry.load(std::memory_order_acquire);
		if (queue == VK_NULL_HANDLE)
			continue;
		uint32_t count = 0;
		gVkGetQueueCheckpointData2NV(queue, &count, nullptr);
		std::vector<VkCheckpointData2NV> checkpoints(count, VkCheckpointData2NV{.sType = VK_STRUCTURE_TYPE_CHECKPOINT_DATA_2_NV});
		gVkGetQueueCheckpointData2NV(queue, &count, checkpoints.data());
		for (const auto& checkpoint : checkpoints)
		{
			auto marker = reinterpret_cast<uintptr_t>(checkpoint.pCheckpointMarker);
			auto sequence = static_cast<uint64_t>(marker >> 1U);
			const auto& slot = gSlots[sequence % kSlotCount];
			// a marker of ours (a layer may add its own): its slot still holds it
			bool ours = sequence != 0 && slot.sequence.load(std::memory_order_acquire) == sequence;
			if (ours && (marker & 1U) == 0U && checkpoint.stage == VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT)
				inFlight.push_back(Crumb{.sequence = sequence, .frame = slot.frame, .name = NameOf(slot.name), .began = true});
			json += std::format(
				"{}\n    {{\"queue\": \"{}\", \"stage\": \"{}\", \"pass\": \"{}\", \"edge\": \"{}\", \"frame\": {}}}",
				json.empty() ? "" : ",", static_cast<void*>(queue), string_VkPipelineStageFlags2(checkpoint.stage),
				ours ? Escape(NameOf(slot.name)) : std::string("?"), (marker & 1U) != 0U ? "end" : "begin",
				ours ? slot.frame : 0);
		}
	}
	return json;
}

// the faults the device reports (VK_EXT_device_fault), as json, and the first's address type (for the signature); its
// vendor binary saved beside the report
std::string ReadDeviceFault(const std::filesystem::path& reportPath, std::string& firstType)
{
	std::string json;
	if (!gCapabilities.deviceFault || gGetDeviceFaultInfo == nullptr)
		return json;

	VkDeviceFaultCountsEXT counts{.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
	if (gGetDeviceFaultInfo(gDevice, &counts, nullptr) != VK_SUCCESS)
		return json;
	if (!gCapabilities.deviceFaultVendorBinary)
		counts.vendorBinarySize = 0;

	std::vector<VkDeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
	std::vector<VkDeviceFaultVendorInfoEXT> vendors(counts.vendorInfoCount);
	std::vector<std::byte> binary(counts.vendorBinarySize);
	VkDeviceFaultInfoEXT info{
		.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT,
		.pAddressInfos = addresses.data(),
		.pVendorInfos = vendors.data(),
		.pVendorBinaryData = binary.empty() ? nullptr : binary.data()};
	if (gGetDeviceFaultInfo(gDevice, &counts, &info) != VK_SUCCESS)
		return json;

	json += std::format("\"description\": \"{}\",\n    \"addresses\": [", Escape(info.description));
	for (size_t addressIt = 0; addressIt < addresses.size(); addressIt++)
	{
		const auto& address = addresses[addressIt];
		if (addressIt == 0)
			firstType = string_VkDeviceFaultAddressTypeKHR(address.addressType);
		// the objects bound around it (the address is only known to its precision, a power of two)
		auto precision = std::max<uint64_t>(address.addressPrecision, 1);
		auto lower = address.reportedAddress & ~(precision - 1);
		auto upper = lower + precision;
		// what is bound there, and what was until recently (a write after a free)
		std::string objects;
		auto describe = [&objects, lower, upper, now = Seconds()](const Binding& binding)
		{
			if (binding.base >= upper || lower >= binding.base + binding.size)
				return;
			objects += std::format(
				"{}{{\"type\": \"{}\", \"handle\": \"{:#x}\", \"name\": \"{}\", \"base\": \"{:#x}\", \"size\": {}{}}}",
				objects.empty() ? "" : ", ", string_VkObjectType(binding.type), binding.handle, Escape(binding.name), binding.base,
				binding.size,
				binding.unboundSeconds < 0.0 ? std::string{} : std::format(", \"unboundSecondsAgo\": {:.3f}", now - binding.unboundSeconds));
		};
		{
			std::scoped_lock lock(gBindingMutex);
			for (const auto& [base, binding] : gBindings)
				describe(binding);
			for (auto unboundIt = gUnboundTotal > kUnboundCount ? gUnboundTotal - kUnboundCount : 0; unboundIt < gUnboundTotal; unboundIt++)
				describe(gUnbound[unboundIt % kUnboundCount]);
		}
		json += std::format(
			"{}\n      {{\"type\": \"{}\", \"address\": \"{:#x}\", \"precision\": {}, \"objects\": [{}]}}",
			addressIt == 0 ? "" : ",", string_VkDeviceFaultAddressTypeKHR(address.addressType), address.reportedAddress,
			address.addressPrecision, objects);
	}
	json += "],\n    \"vendor\": [";
	for (size_t vendorIt = 0; vendorIt < vendors.size(); vendorIt++)
		json += std::format(
			"{}\n      {{\"description\": \"{}\", \"code\": \"{:#x}\", \"data\": \"{:#x}\"}}", vendorIt == 0 ? "" : ",",
			Escape(vendors[vendorIt].description), vendors[vendorIt].vendorFaultCode, vendors[vendorIt].vendorFaultData);
	json += "]";
	if (!binary.empty() && !reportPath.empty())
	{
		auto binaryPath = std::filesystem::path(reportPath).replace_extension(".bin");
		if (core::file::Write(binaryPath, binary))
			json += std::format(",\n    \"vendorBinary\": \"{}\"", Escape(binaryPath.string()));
	}
	return json;
}

// writes the report (kind: "device lost", "hang", or the error) and ends the process. the first caller only: the others
// wait for it
[[noreturn]] void Report(std::string_view kind, VkResult result, VkQueue queue, std::string_view what)
{
	if (gReporting.exchange(true, std::memory_order_acq_rel))
		for (;;)
			std::this_thread::sleep_for(std::chrono::hours(1)); // the reporting thread ends the process

	auto now = std::chrono::system_clock::now();
	auto stamp = std::format("{:%Y%m%d-%H%M%S}", std::chrono::floor<std::chrono::seconds>(now));
	std::filesystem::path path;
	if (!gDirectory.empty())
		path = std::filesystem::path(gDirectory) / std::format("crash-{}.json", stamp);

	// what the gpu had begun and not ended (buffer markers), or reached last (checkpoints): not with the Crash Diagnostic
	// Layer, whose dump says more, and whose checkpoints the queues' are
	bool layer = gCapabilities.crashDiagnosticLayer;
	std::vector<Crumb> crumbs = layer ? std::vector<Crumb>{} : ReadBufferMarkers();
	std::vector<Crumb> checkpointsInFlight;
	std::string checkpoints = layer ? std::string{} : ReadCheckpoints(checkpointsInFlight);
	std::ranges::sort(checkpointsInFlight, {}, &Crumb::sequence);
	std::string faultType;
	std::string fault = (layer || result != VK_ERROR_DEVICE_LOST) ? std::string{} : ReadDeviceFault(path, faultType);

	std::vector<const Crumb*> inFlight;
	std::vector<const Crumb*> completed;
	size_t notBegun = 0;
	for (const auto& crumb : crumbs)
	{
		if (crumb.began && !crumb.ended)
			inFlight.push_back(&crumb);
		else if (crumb.ended)
			completed.push_back(&crumb);
		else
			notBegun++;
	}
	for (const auto& crumb : checkpointsInFlight)
		inFlight.push_back(&crumb);
	std::string suspect = inFlight.empty() ? std::string("none") : inFlight.front()->name;

	// what tells crashes of the same cause from others: the gpu, its driver, the result, the first pass in flight and
	// the kind of fault
	auto signatureText = std::format(
		"{:#x}|{:#x}|{}|{}|{}|{}", gDescription.vendorId, gDescription.driverVersion, kind, string_VkResult(result), suspect,
		faultType);
	auto signature = XXH3_64bits(signatureText.data(), signatureText.size());

	std::string json = "{\n";
	json += std::format("  \"kind\": \"{}\",\n", Escape(kind));
	json += std::format("  \"result\": \"{}\",\n", string_VkResult(result));
	if (!what.empty())
		json += std::format("  \"what\": \"{}\",\n", Escape(what));
	json += std::format("  \"signature\": \"{:016x}\",\n", signature);
	json += std::format("  \"time\": \"{:%Y-%m-%dT%H:%M:%SZ}\",\n", std::chrono::floor<std::chrono::seconds>(now));
	json += std::format("  \"secondsSinceStart\": {:.3f},\n", Seconds());
	json += std::format("  \"frame\": {},\n", gFrame.load(std::memory_order_relaxed));
#ifdef SPEEDO_VERSION
	json += std::format("  \"version\": \"{}\",\n", SPEEDO_VERSION);
#endif
	json += std::format("  \"build\": \"{}\",\n", kBuildConfig);
	json += std::format("  \"platform\": \"{}\",\n", kPlatform);
	json += std::format(
		"  \"device\": {{\"name\": \"{}\", \"vendorId\": \"{:#06x}\", \"deviceId\": \"{:#06x}\", \"driver\": \"{}\", \"driverInfo\": \"{}\", \"driverVersion\": \"{}\", \"api\": \"{}.{}.{}\"}},\n",
		Escape(gDescription.name), gDescription.vendorId, gDescription.deviceId, Escape(gDescription.driverName),
		Escape(gDescription.driverInfo), DriverVersionString(gDescription.vendorId, gDescription.driverVersion),
		VK_API_VERSION_MAJOR(gDescription.apiVersion), VK_API_VERSION_MINOR(gDescription.apiVersion),
		VK_API_VERSION_PATCH(gDescription.apiVersion));
	json += "  \"tools\": [";
	for (size_t toolIt = 0; toolIt < gDescription.tools.size(); toolIt++)
		json += std::format("{}\"{}\"", toolIt == 0 ? "" : ", ", Escape(gDescription.tools[toolIt]));
	json += "],\n";
	if (queue != VK_NULL_HANDLE)
		json += std::format("  \"queue\": \"{}\",\n", static_cast<void*>(queue));
	json += std::format(
		"  \"breadcrumbs\": \"{}\",\n",
		layer ? "the Crash Diagnostic Layer's dump" : gCapabilities.bufferMarker ? "buffer markers" : gCapabilities.checkpoints ? "checkpoints" : "none");

	auto crumbJson = [](const Crumb& crumb)
	{ return std::format("{{\"pass\": \"{}\", \"frame\": {}, \"sequence\": {}}}", Escape(crumb.name), crumb.frame, crumb.sequence); };
	json += "  \"inFlight\": [";
	for (size_t crumbIt = 0; crumbIt < inFlight.size(); crumbIt++)
		json += std::format("{}\n    {}", crumbIt == 0 ? "" : ",", crumbJson(*inFlight[crumbIt]));
	json += "],\n";
	json += "  \"lastCompleted\": [";
	constexpr size_t kCompletedCount = 8;
	for (size_t crumbIt = completed.size() > kCompletedCount ? completed.size() - kCompletedCount : 0, first = crumbIt;
		 crumbIt < completed.size(); crumbIt++)
		json += std::format("{}\n    {}", crumbIt == first ? "" : ",", crumbJson(*completed[crumbIt]));
	json += "],\n";
	json += std::format("  \"notBegun\": {},\n", notBegun);
	json += std::format("  \"checkpoints\": [{}],\n", checkpoints);
	json += std::format("  \"deviceFault\": {{{}}},\n", fault.empty() ? std::string{} : "\n    " + fault + "\n  ");

	json += "  \"submits\": [";
	{
		std::scoped_lock lock(gSubmitMutex);
		auto first = gSubmitTotal > kSubmitCount ? gSubmitTotal - kSubmitCount : 0;
		for (auto submitIt = first; submitIt < gSubmitTotal; submitIt++)
		{
			const auto& submit = gSubmits[submitIt % kSubmitCount];
			json += std::format(
				"{}\n    {{\"seconds\": {:.3f}, \"frame\": {}, \"queue\": \"{}\", \"commandBuffers\": {}, \"timeline\": {}}}",
				submitIt == first ? "" : ",", submit.seconds, submit.frame, static_cast<void*>(submit.queue),
				submit.commandBufferCount, submit.timelineValue);
		}
	}
	json += "],\n";
	json += "  \"events\": [";
	{
		std::scoped_lock lock(gEventMutex);
		auto first = gEventTotal > kEventCount ? gEventTotal - kEventCount : 0;
		for (auto eventIt = first; eventIt < gEventTotal; eventIt++)
		{
			const auto& event = gEvents[eventIt % kEventCount];
			json += std::format(
				"{}\n    {{\"seconds\": {:.3f}, \"frame\": {}, \"what\": \"{}\"}}", eventIt == first ? "" : ",", event.seconds,
				event.frame, Escape(event.what));
		}
	}
	json += "]\n}\n";

	bool written = false;
	if (!path.empty())
	{
		std::error_code error;
		std::filesystem::create_directories(path.parent_path(), error);
		written = core::file::Write(path, std::as_bytes(std::span(json))).has_value();
	}

	std::println(
		stderr, "crash report: {} ({}), signature {:016x}, in flight: {}{}", kind, string_VkResult(result), signature, suspect,
		written ? std::format(", written to {}", path.string()) : std::string(", not written"));
	if (!written)
		std::println(stderr, "{}", json);
	if (layer)
	{
		// the layer writes its dump on the thread whose call met the loss first, which may be another: let it finish
		std::println(stderr, "crash report: the Crash Diagnostic Layer's dump has the commands in flight");
		std::fflush(stderr);
		std::this_thread::sleep_for(std::chrono::seconds(3));
	}
	std::fflush(stderr);
	std::fflush(stdout);
	TRAP();
}

} // namespace detail

bool WantsDebugUtils(VkInstance /*instance*/)
{
	static const bool kHas = detail::HasInstanceExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
	return kHas;
}

void CreateMessenger(VkInstance instance, const VkAllocationCallbacks* allocator)
{
	using namespace detail;

	auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
		vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
	if (create == nullptr)
		return;
	VkDebugUtilsMessengerCreateInfoEXT info{
		.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
		.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT,
		.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT,
		.pfnUserCallback = OnAddressBinding};
	if (create(instance, &info, allocator, &gMessenger) != VK_SUCCESS)
		gMessenger = VK_NULL_HANDLE;
}

void DestroyMessenger(VkInstance instance, const VkAllocationCallbacks* allocator)
{
	using namespace detail;

	if (gMessenger == VK_NULL_HANDLE)
		return;
	auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
		vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
	if (destroy != nullptr)
		destroy(instance, gMessenger, allocator);
	gMessenger = VK_NULL_HANDLE;
}

void Detect(VkInstance instance, VkPhysicalDevice physicalDevice)
{
	using namespace detail;

	auto& capabilities = gCapabilities;
	capabilities = {};
	gDescription.tools.clear();

	// the tools loaded (layers name themselves here): the Crash Diagnostic Layer reports instead
	if (auto getTools = reinterpret_cast<PFN_vkGetPhysicalDeviceToolPropertiesEXT>(
			vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceToolPropertiesEXT"));
		getTools != nullptr)
	{
		uint32_t count = 0;
		getTools(physicalDevice, &count, nullptr);
		std::vector<VkPhysicalDeviceToolPropertiesEXT> tools(count, VkPhysicalDeviceToolPropertiesEXT{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TOOL_PROPERTIES_EXT});
		getTools(physicalDevice, &count, tools.data());
		for (const auto& tool : tools)
		{
			gDescription.tools.emplace_back(std::format("{} {} ({})", tool.name, tool.version, tool.layer));
			if (std::strcmp(tool.layer, kCrashDiagnosticLayerName) == 0)
				capabilities.crashDiagnosticLayer = true;
		}
	}

	VkPhysicalDeviceCoherentMemoryFeaturesAMD coherent{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COHERENT_MEMORY_FEATURES_AMD};
	VkPhysicalDeviceAddressBindingReportFeaturesEXT binding{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ADDRESS_BINDING_REPORT_FEATURES_EXT, .pNext = &coherent};
	VkPhysicalDeviceFaultFeaturesEXT fault{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT, .pNext = &binding};
	VkPhysicalDeviceFeatures2 features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &fault};
	vkGetPhysicalDeviceFeatures2(physicalDevice, &features);

	bool layer = capabilities.crashDiagnosticLayer;
	// SPEEDO_CRASH_BREADCRUMBS=markers|checkpoints|none: one way only (if the device has it), to check it
	std::string_view only;
	if (const char* setting = std::getenv("SPEEDO_CRASH_BREADCRUMBS"); setting != nullptr)
		only = setting;
	capabilities.bufferMarker =
		!layer && (only.empty() || only == "markers") && SupportsExtension(VK_AMD_BUFFER_MARKER_EXTENSION_NAME, physicalDevice);
	capabilities.checkpoints = !layer && !capabilities.bufferMarker && (only.empty() || only == "checkpoints") &&
							   SupportsExtension(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME, physicalDevice);
	capabilities.deviceFault = SupportsExtension(VK_EXT_DEVICE_FAULT_EXTENSION_NAME, physicalDevice) && fault.deviceFault != VK_FALSE;
	capabilities.deviceFaultVendorBinary = capabilities.deviceFault && fault.deviceFaultVendorBinary != VK_FALSE;
	capabilities.addressBindingReport = SupportsExtension(VK_EXT_DEVICE_ADDRESS_BINDING_REPORT_EXTENSION_NAME, physicalDevice) &&
										WantsDebugUtils(instance) && binding.reportAddressBinding != VK_FALSE;
	capabilities.deviceCoherentMemory = capabilities.bufferMarker &&
										SupportsExtension(VK_AMD_DEVICE_COHERENT_MEMORY_EXTENSION_NAME, physicalDevice) &&
										coherent.deviceCoherentMemory != VK_FALSE;

	std::println(
		"crash reports: breadcrumbs by {}, device fault {}, address bindings {}{}",
		layer ? "the Crash Diagnostic Layer" : capabilities.bufferMarker ? "buffer markers" : capabilities.checkpoints ? "checkpoints" : "nothing",
		capabilities.deviceFault ? "yes" : "no", capabilities.addressBindingReport && gMessenger != VK_NULL_HANDLE ? "yes" : "no",
		capabilities.deviceCoherentMemory ? ", device coherent" : "");
}

const Capabilities& GetCapabilities() noexcept
{
	return detail::gCapabilities;
}

bool ChainsFeatures(const char* extension, VkInstance instance, VkPhysicalDevice physicalDevice)
{
	if (!SupportsExtension(extension, physicalDevice))
		return false;
	// it needs the instance's VK_EXT_debug_utils
	if (std::strcmp(extension, VK_EXT_DEVICE_ADDRESS_BINDING_REPORT_EXTENSION_NAME) == 0)
		return WantsDebugUtils(instance);
	return true;
}

void AddDeviceExtensions(std::vector<const char*>& extensions, VkInstance instance, VkPhysicalDevice physicalDevice)
{
	const auto& capabilities = detail::gCapabilities;
	if (capabilities.bufferMarker)
		extensions.emplace_back(VK_AMD_BUFFER_MARKER_EXTENSION_NAME);
	if (capabilities.checkpoints)
		extensions.emplace_back(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
	// wherever their features are in the device's chain (see ChainsFeatures), used or not
	for (const char* extension : {VK_EXT_DEVICE_FAULT_EXTENSION_NAME, VK_EXT_DEVICE_ADDRESS_BINDING_REPORT_EXTENSION_NAME, VK_AMD_DEVICE_COHERENT_MEMORY_EXTENSION_NAME})
		if (ChainsFeatures(extension, instance, physicalDevice))
			extensions.emplace_back(extension);
}

void Init(Device<kVk>& device)
{
	using namespace detail;

	if (gDevice != VK_NULL_HANDLE)
		return; // the primary device's only

	gDevice = device;
	gAllocator = device.GetAllocator();

	const auto& info = device.GetInstance().GetPhysicalDeviceInfo(device.GetPhysicalDevice());
	const auto& properties = info.deviceProperties.properties;
	gDescription.name = properties.deviceName;
	gDescription.vendorId = properties.vendorID;
	gDescription.deviceId = properties.deviceID;
	gDescription.driverVersion = properties.driverVersion;
	gDescription.apiVersion = properties.apiVersion;
	gDescription.driverName = info.deviceProperties12Ex.driverName;
	gDescription.driverInfo = info.deviceProperties12Ex.driverInfo;

	if (gCapabilities.deviceFault)
		gGetDeviceFaultInfo = reinterpret_cast<PFN_vkGetDeviceFaultInfoEXT>(vkGetDeviceProcAddr(gDevice, "vkGetDeviceFaultInfoEXT"));

	if (gCapabilities.bufferMarker)
	{
		gWriteBufferMarker = reinterpret_cast<PFN_vkCmdWriteBufferMarkerAMD>(vkGetDeviceProcAddr(gDevice, "vkCmdWriteBufferMarkerAMD"));
		// host visible, coherent (and device coherent and uncached where the device has it: its writes reach the host as
		// they happen, rather than when caches are flushed, which a lost device never does)
		VkMemoryPropertyFlags memoryFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
		if (gCapabilities.deviceCoherentMemory)
			memoryFlags |= VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD | VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD;
		std::tie(gMarkerBuffer, gMarkerMemory) = CreateBuffer(
			gAllocator, sizeof(uint32_t) * 2 * kSlotCount, VK_BUFFER_USAGE_TRANSFER_DST_BIT, memoryFlags, "Crash Breadcrumbs");
		void* data = nullptr;
		VK_CHECK(vmaMapMemory(gAllocator, gMarkerMemory, &data));
		gMarkers = static_cast<volatile uint32_t*>(data);
		for (uint32_t markerIt = 0; markerIt < 2 * kSlotCount; markerIt++)
			gMarkers[markerIt] = 0;
		if (gWriteBufferMarker == nullptr)
			gCapabilities.bufferMarker = false;
	}
}

void Shutdown()
{
	using namespace detail;

	if (gMarkerBuffer != VK_NULL_HANDLE)
	{
		vmaUnmapMemory(gAllocator, gMarkerMemory);
		DestroyBuffer(gAllocator, gMarkerBuffer, gMarkerMemory);
	}
	gMarkers = nullptr;
	gMarkerBuffer = VK_NULL_HANDLE;
	gMarkerMemory = VK_NULL_HANDLE;
	gCapabilities.bufferMarker = false;
	gCapabilities.checkpoints = false;
	gCapabilities.deviceFault = false;
	gGetDeviceFaultInfo = nullptr;
	gAllocator = VK_NULL_HANDLE;
	gDevice = VK_NULL_HANDLE;
}

void OnSubmit(VkQueue queue, uint32_t commandBufferCount, uint64_t timelineValue)
{
	using namespace detail;

	{
		std::scoped_lock lock(gSubmitMutex);
		gSubmits[gSubmitTotal++ % kSubmitCount] = Submit{
			.seconds = Seconds(),
			.frame = gFrame.load(std::memory_order_relaxed),
			.queue = queue,
			.commandBufferCount = commandBufferCount,
			.timelineValue = timelineValue};
	}

	// the queues seen, once each
	for (auto& entry : gQueues)
	{
		auto* current = entry.load(std::memory_order_acquire);
		if (current == queue)
			return;
		if (current == VK_NULL_HANDLE)
		{
			VkQueue expected = VK_NULL_HANDLE;
			if (entry.compare_exchange_strong(expected, queue, std::memory_order_acq_rel) || expected == queue)
				return;
		}
	}
}

void ReportFatal(VkResult result, VkQueue queue)
{
	detail::Report(result == VK_ERROR_DEVICE_LOST ? "device lost" : "error", result, queue, {});
}

void ReportHang(std::string_view what)
{
	detail::Report("hang", VK_TIMEOUT, VK_NULL_HANDLE, what);
}

uint64_t HangTimeout() noexcept
{
	static const uint64_t kTimeout = []
	{
		constexpr uint64_t kDefaultSeconds = 20;
		constexpr int kDecimal = 10;
		uint64_t seconds = kDefaultSeconds;
		if (const char* setting = std::getenv("SPEEDO_GPU_HANG_TIMEOUT"); setting != nullptr && *setting != '\0')
			seconds = std::strtoull(setting, nullptr, kDecimal);
		return seconds == 0 ? ~0ULL : seconds * detail::kNanosecondsPerSecond;
	}();
	return kTimeout;
}

} // namespace rhi::vk::crash

namespace rhi
{

void SetCrashReportDirectory(std::string_view directory)
{
	vk::crash::detail::gDirectory = directory;
}

void CrashEvent(std::string_view what)
{
	using namespace vk::crash::detail;

	std::scoped_lock lock(gEventMutex);
	auto& event = gEvents[gEventTotal++ % kEventCount];
	event.seconds = Seconds();
	event.frame = gFrame.load(std::memory_order_relaxed);
	event.what = what;
}

void CrashFrame(uint64_t frame)
{
	vk::crash::detail::gFrame.store(frame, std::memory_order_relaxed);
}

void CrashHeartbeat()
{
	using namespace vk::crash::detail;

	auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - kStart).count();
	gHeartbeat.store(std::max<int64_t>(now, 1), std::memory_order_relaxed);
}

void CrashWatch()
{
	using namespace vk::crash::detail;

	auto heartbeat = gHeartbeat.load(std::memory_order_relaxed);
	auto timeout = vk::crash::HangTimeout();
	if (heartbeat == 0 || timeout == ~0ULL)
		return;
	auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - kStart).count();
	if (now > heartbeat && static_cast<uint64_t>(now - heartbeat) > timeout)
		vk::crash::ReportHang(std::format("the draw thread hasn't drawn or skipped a frame for {} s", timeout / kNanosecondsPerSecond));
}

template <>
CrashBreadcrumb<kVk>::CrashBreadcrumb(CommandBufferHandle<kVk> cmd, std::string_view name)
	: myCmd(cmd)
{
	using namespace vk::crash::detail;

	const auto& capabilities = gCapabilities;
	if (!capabilities.bufferMarker && !capabilities.checkpoints)
		return;

	auto sequence = gNextSequence.fetch_add(1, std::memory_order_relaxed);
	auto slotIndex = static_cast<uint32_t>(sequence % kSlotCount);
	auto& slot = gSlots[slotIndex];
	slot.name = InternName(name);
	slot.frame = gFrame.load(std::memory_order_relaxed);
	slot.sequence.store(sequence, std::memory_order_release);
	mySequence = sequence;

	if (capabilities.bufferMarker)
	{
		// the gpu is long done with the slot's last use (see kSlotCount): cleared for this one
		gMarkers[static_cast<size_t>(slotIndex) * 2] = 0;
		gMarkers[(static_cast<size_t>(slotIndex) * 2) + 1] = 0;
		gWriteBufferMarker(
			cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, gMarkerBuffer, sizeof(uint32_t) * slotIndex * 2, static_cast<uint32_t>(sequence));
	}
	else
		gVkCmdSetCheckpointNV(cmd, EncodeCheckpoint(sequence, false));
}

template <>
void CrashBreadcrumb<kVk>::End()
{
	using namespace vk::crash::detail;

	if (mySequence == 0)
		return;

	if (gCapabilities.bufferMarker)
		gWriteBufferMarker(
			myCmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, gMarkerBuffer,
			sizeof(uint32_t) * (((mySequence % kSlotCount) * 2) + 1), static_cast<uint32_t>(mySequence));
	else if (gCapabilities.checkpoints)
		gVkCmdSetCheckpointNV(myCmd, EncodeCheckpoint(mySequence, true));
	mySequence = 0;
}

} // namespace rhi
