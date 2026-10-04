#include <rhi/deviceobject.h>
#include <rhi/vulkan/utils.h>

#include <core/profiling.h>

#if (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
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

} // namespace deviceobject

template <>
void TrackObject<kVk>(VkDevice device, VkObjectType type, uint64_t handle, std::string_view name)
{
	using namespace deviceobject;

	ZoneScopedN("TrackObject");

	// a null handle means the object was never created: tracking it is a bug at the call site
	ENSUREF(handle != 0U, "tracking a null handle: type {}", static_cast<int>(type));

	gObjects.try_emplace_l({type, handle}, [](auto& object) { ++object.second; }, 1U);
	gTypeCounts.try_emplace_l(type, [](auto& count) { ++count.second; }, 1U);

	// instance level objects are created before there is a device to name them with
	if (device != VK_NULL_HANDLE && !name.empty())
	{
		ZoneScopedN("TrackObject::vkSetDebugUtilsObjectNameEXT");

		std::string nameString(name); // null terminated
		VkDebugUtilsObjectNameInfoEXT nameInfo{
			.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
			.pNext = nullptr,
			.objectType = type,
			.objectHandle = handle,
			.pObjectName = nameString.c_str()};
		VK_CHECK(gVkSetDebugUtilsObjectNameExt(device, &nameInfo));
	}
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
