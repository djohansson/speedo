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
// there is one: the last one passed to NameInstanceObjects, until it is untracked
static ParallelMap<std::pair<VkObjectType, uint64_t>, std::string> gInstanceObjectNames;
static std::atomic<VkDevice> gNamingDevice = VK_NULL_HANDLE;

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

	if (auto* device = gNamingDevice.load(std::memory_order_acquire); device != VK_NULL_HANDLE)
		InternalSetName(device, type, handle, name);
}

template <>
void NameInstanceObjects<kVk>(VkDevice device)
{
	using namespace deviceobject;

	ZoneScopedN("NameInstanceObjects");

	ENSURE(device != VK_NULL_HANDLE);

	gNamingDevice.store(device, std::memory_order_release);
	gInstanceObjectNames.for_each([device](const auto& object)
	{
		const auto& [key, name] = object;
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
#endif // SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0

} // namespace rhi
