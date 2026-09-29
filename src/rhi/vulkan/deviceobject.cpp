#include <rhi/deviceobject.h>
#include <rhi/vulkan/utils.h>

namespace rhi
{

#if (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
template <>
void AddOwnedObjectHandle<kVk>(
	DeviceHandle<kVk> device,
	const uuids::uuid& ownerId,
	ObjectType<kVk> objectType,
	uint64_t objectHandle,
	std::string_view objectName)
{
	ZoneScopedN("AddOwnedObjectHandle");

	if (objectHandle == 0U)
		return;

	uint64_t ownerIdHash = 0ULL;

	{
		ZoneScopedN("AddOwnedObjectHandle::hash");

		ownerIdHash = XXH3_64bits(&ownerId, sizeof(ownerId));
	}

	{
		auto lock = std::scoped_lock(gObjectNameInfoMutex);

		auto& objectInfos = gOwnerToObjectNameInfoMap<kVk>[ownerIdHash];

		auto& [objectInfo, objectNameStr] = objectInfos.emplace_back(
			std::make_pair(
				ObjectNameInfo<kVk>{
					.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
					.pNext = nullptr,
					.objectType = objectType,
					.objectHandle = objectHandle,
				},
				std::make_unique<char[]>(objectName.size() + 1))); //NOLINT(modernize-avoid-c-arrays)
		objectName.copy(objectNameStr.get(), objectName.size());
		objectInfo.pObjectName = objectNameStr.get();

		{
			ZoneScopedN("AddOwnedObjectHandle::vkSetDebugUtilsObjectNameEXT");

			VK_CHECK(gVkSetDebugUtilsObjectNameExt(device, &objectInfo));
		}

		gObjectTypeToCountMap<kVk>[objectType]++;
	}
}
#endif // SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0

} // namespace rhi
