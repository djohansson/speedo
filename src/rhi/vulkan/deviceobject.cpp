#include <array>
#include <rhi/deviceobject.h>
#include <rhi/vulkan/utils.h>

#include <core/profiling.h>

#if (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
#include <atomic>
#include <mutex>
#include <string>
#include <utility>

#include <parallel_hashmap/phmap.h>
#endif

namespace rhi
{

#if (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
namespace deviceobject
{

// sharded maps with a lock per shard, so objects of different types/handles are mostly tracked without contention.
// keyed by type and handle: non-dispatchable handles are only unique per type, and two live objects may even share
// a handle (e.g. identical immutable samplers), hence the reference count.
template <typename K, typename V>
using ParallelMap = phmap::parallel_flat_hash_map<K, V, phmap::priv::hash_default_hash<K>, phmap::priv::hash_default_eq<K>, std::allocator<std::pair<const K, V>>, 4, std::mutex>;

static ParallelMap<std::pair<VkObjectType, uint64_t>, uint32_t> gObjects;
static ParallelMap<VkObjectType, uint32_t> gTypeCounts;
// the names of the instance level objects (see TrackInstanceObject), and the device they are named through once
// there is one: the last one passed to NameInstanceObjects, until it is untracked, and that device's physical device
static ParallelMap<std::pair<VkObjectType, uint64_t>, std::string> gInstanceObjectNames;
static std::atomic<VkDevice> gNamingDevice = VK_NULL_HANDLE;
static std::atomic<VkPhysicalDevice> gNamingPhysicalDevice = VK_NULL_HANDLE;

// whether an instance level object can be named through a device created from physicalDevice: every object except
// other physical devices, which may belong to another driver (see NameInstanceObjects in rhi/deviceobject.h)
static bool InternalCanNameThrough(VkObjectType type, uint64_t handle, VkPhysicalDevice physicalDevice)
{
	return type != VK_OBJECT_TYPE_PHYSICAL_DEVICE || handle == reinterpret_cast<uint64_t>(physicalDevice);
}

static void InternalTrack(VkObjectType type, uint64_t handle, std::string_view name)
{
	// a null handle means the object was never created: tracking it is a bug at the call site
	ENSUREF(handle != 0U, "tracking a null handle: type {}", static_cast<int>(type));
	ENSUREF(!name.empty(), "tracking an object without a name: type {}, handle {:#x}", static_cast<int>(type), handle);

	gObjects.try_emplace_l({type, handle}, [](auto& object) { ++object.second; }, 1U);
	gTypeCounts.try_emplace_l(type, [](auto& count) { ++count.second; }, 1U);
}

static void InternalSetName(VkDevice device, VkObjectType type, uint64_t handle, std::string_view name)
{
	ZoneScopedN("vkSetDebugUtilsObjectNameEXT");

	std::string nameString(name); // null terminated
	VkDebugUtilsObjectNameInfoEXT nameInfo{
		.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
		.pNext = nullptr,
		.objectType = type,
		.objectHandle = handle,
		.pObjectName = nameString.c_str()};
	VK_CHECK(gVkSetDebugUtilsObjectNameExt(device, &nameInfo));
}

} // namespace deviceobject

template <>
void TrackObject<kVk>(VkDevice device, VkObjectType type, uint64_t handle, std::string_view name)
{
	using namespace deviceobject;

	ZoneScopedN("TrackObject");

	ENSUREF(device != VK_NULL_HANDLE, "tracking an object without a device: type {} (see TrackInstanceObject)", static_cast<int>(type));

	InternalTrack(type, handle, name);
	InternalSetName(device, type, handle, name);
}

template <>
void TrackInstanceObject<kVk>(VkObjectType type, uint64_t handle, std::string_view name)
{
	using namespace deviceobject;

	ZoneScopedN("TrackInstanceObject");

	InternalTrack(type, handle, name);
	gInstanceObjectNames.try_emplace({type, handle}, name);

	if (auto* device = gNamingDevice.load(std::memory_order_acquire); device != VK_NULL_HANDLE &&
		InternalCanNameThrough(type, handle, gNamingPhysicalDevice.load(std::memory_order_acquire)))
		InternalSetName(device, type, handle, name);
}

template <>
void NameInstanceObjects<kVk>(VkDevice device, VkPhysicalDevice physicalDevice)
{
	using namespace deviceobject;

	ZoneScopedN("NameInstanceObjects");

	ENSURE(device != VK_NULL_HANDLE);
	ENSURE(physicalDevice != VK_NULL_HANDLE);

	gNamingPhysicalDevice.store(physicalDevice, std::memory_order_release);
	gNamingDevice.store(device, std::memory_order_release);
	gInstanceObjectNames.for_each([device, physicalDevice](const auto& object)
	{
		const auto& [key, name] = object;
		if (InternalCanNameThrough(key.first, key.second, physicalDevice))
			InternalSetName(device, key.first, key.second, name);
	});
}

template <>
void UntrackObject<kVk>(VkObjectType type, uint64_t handle)
{
	using namespace deviceobject;

	ZoneScopedN("UntrackObject");

	// callers destroying an object that may not exist (e.g. moved from) check that themselves, like for vkDestroy*
	ENSUREF(handle != 0U, "untracking a null handle: type {}", static_cast<int>(type));

	bool tracked = false;
	gObjects.erase_if({type, handle}, [&tracked](auto& object) { tracked = true; return --object.second == 0; });
	ENSUREF(tracked, "untracking an object that was not tracked: type {}, handle {:#x}", static_cast<int>(type), handle);

	gTypeCounts.modify_if(type, [](auto& count) { --count.second; });

	gInstanceObjectNames.erase({type, handle});

	// stop naming instance level objects through a device that is going away
	if (type == VK_OBJECT_TYPE_DEVICE)
	{
		auto* device = reinterpret_cast<VkDevice>(handle);
		gNamingDevice.compare_exchange_strong(device, VK_NULL_HANDLE, std::memory_order_acq_rel);
	}
}

template <>
uint32_t GetTypeCount<kVk>(VkObjectType type)
{
	uint32_t count = 0;
	deviceobject::gTypeCounts.if_contains(type, [&count](const auto& entry) { count = entry.second; });
	return count;
}

template <>
std::vector<std::pair<std::string_view, uint32_t>> GetObjectCounts<kVk>()
{
	static constexpr std::array<std::pair<std::string_view, VkObjectType>, 28> kTypes{{
		{"Instances", VK_OBJECT_TYPE_INSTANCE},
		{"Physical Devices", VK_OBJECT_TYPE_PHYSICAL_DEVICE},
		{"Devices", VK_OBJECT_TYPE_DEVICE},
		{"Queues", VK_OBJECT_TYPE_QUEUE},
		{"Semaphores", VK_OBJECT_TYPE_SEMAPHORE},
		{"Command Buffers", VK_OBJECT_TYPE_COMMAND_BUFFER},
		{"Fences", VK_OBJECT_TYPE_FENCE},
		{"Device Memory", VK_OBJECT_TYPE_DEVICE_MEMORY},
		{"Buffers", VK_OBJECT_TYPE_BUFFER},
		{"Images", VK_OBJECT_TYPE_IMAGE},
		{"Events", VK_OBJECT_TYPE_EVENT},
		{"Query Pools", VK_OBJECT_TYPE_QUERY_POOL},
		{"Buffer Views", VK_OBJECT_TYPE_BUFFER_VIEW},
		{"Image Views", VK_OBJECT_TYPE_IMAGE_VIEW},
		{"Shader Modules", VK_OBJECT_TYPE_SHADER_MODULE},
		{"Pipeline Caches", VK_OBJECT_TYPE_PIPELINE_CACHE},
		{"Pipeline Layouts", VK_OBJECT_TYPE_PIPELINE_LAYOUT},
		{"Render Passes", VK_OBJECT_TYPE_RENDER_PASS},
		{"Pipelines", VK_OBJECT_TYPE_PIPELINE},
		{"Descriptor Set Layouts", VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT},
		{"Samplers", VK_OBJECT_TYPE_SAMPLER},
		{"Descriptor Pools", VK_OBJECT_TYPE_DESCRIPTOR_POOL},
		{"Descriptor Sets", VK_OBJECT_TYPE_DESCRIPTOR_SET},
		{"Descriptor Update Templates", VK_OBJECT_TYPE_DESCRIPTOR_UPDATE_TEMPLATE},
		{"Framebuffers", VK_OBJECT_TYPE_FRAMEBUFFER},
		{"Command Pools", VK_OBJECT_TYPE_COMMAND_POOL},
		{"Surfaces", VK_OBJECT_TYPE_SURFACE_KHR},
		{"Swapchains", VK_OBJECT_TYPE_SWAPCHAIN_KHR},
	}};

	std::vector<std::pair<std::string_view, uint32_t>> counts;
	counts.reserve(kTypes.size());
	for (const auto& [name, type] : kTypes)
		counts.emplace_back(name, GetTypeCount<kVk>(type));
	return counts;
}
#endif // SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0

} // namespace rhi
