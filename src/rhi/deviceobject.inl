#include <core/profiling.h>

#include <xxh3.h>

#include <shared_mutex>

namespace rhi
{

#if (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
template <GraphicsApi G>
void EraseOwnedObjectHandle(const uuids::uuid& ownerId, uint64_t objectHandle)
{
	ZoneScopedN("EraseOwnedObjectHandle");

	if (objectHandle == 0U)
		return;

	uint64_t ownerIdHash = 0ULL;

	{
		ZoneScopedN("EraseOwnedObjectHandle::hash");

		ownerIdHash = XXH3_64bits(&ownerId, sizeof(ownerId));
	}

	{
		ZoneScopedN("EraseOwnedObjectHandle::erase");

		auto lock = std::scoped_lock(gObjectNameInfoMutex);

		auto& objectInfos = gOwnerToObjectNameInfoMap<G>[ownerIdHash];

		for (auto it = objectInfos.begin(); it != objectInfos.end(); it++)
		{
			if (it->first.objectHandle == objectHandle)
			{
				gObjectTypeToCountMap<G>[it->first.objectType]--;
				objectInfos.erase(it);
				return;
			}
		}
	}
}

template <GraphicsApi G>
void ClearOwnedObjectHandles(const uuids::uuid& ownerId)
{
	ZoneScopedN("ClearOwnedObjectHandles");

	uint64_t ownerIdHash = 0ULL;

	{
		ZoneScopedN("ClearOwnedObjectHandles::hash");

		ownerIdHash = XXH3_64bits(&ownerId, sizeof(ownerId));
	}

	{
		ZoneScopedN("ClearOwnedObjectHandles::clear");

		auto lock = std::scoped_lock(gObjectNameInfoMutex);

		auto& objectInfos = gOwnerToObjectNameInfoMap<G>[ownerIdHash];

		for (const auto& [objectInfo, objectName] : objectInfos)
			gObjectTypeToCountMap<G>[objectInfo.objectType]--;

		objectInfos.clear();
	}
}

template <GraphicsApi G>
uint32_t GetTypeCount(ObjectType<G> type)
{
	std::shared_lock lock(gObjectNameInfoMutex);

	return gObjectTypeToCountMap<G>[type];
}
#endif // SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0


template <typename DerivedType>
DeviceObject<DerivedType>::DeviceObject(DeviceObject<DerivedType>&& other) noexcept
{
	Swap(other);
}

template <typename DerivedType>
DeviceObject<DerivedType>::DeviceObject(CreateDescType&& desc)
	: SuperType(std::forward<CreateDescType>(desc))
{
#if (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
	{
		for (const auto* objectPtr : SuperType::GetDesc().objectPointers)
			AddOwnedObjectHandle<GetApi()>(
				GetDevice(),
				SuperType::GetDesc().uuid,
				SuperType::GetDesc().objectType,
				*objectPtr,
				std::format("{}", uuids::to_string(SuperType::GetDesc().uuid)));
	}
#endif
}

template <typename DerivedType>
DeviceObject<DerivedType>::~DeviceObject()
{
#if (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
	{
		if (SuperType::IsValid())
			ClearOwnedObjectHandles<GetApi()>(SuperType::GetDesc().uuid);
	}
#endif
}

template <typename DerivedType>
DeviceObject<DerivedType>& DeviceObject<DerivedType>::operator=(DeviceObject<DerivedType>&& other) noexcept
{
	Swap(other);
	return *this;
}

template <typename DerivedType>
void DeviceObject<DerivedType>::Swap(DeviceObject<DerivedType>& other) noexcept
{
	SuperType::Swap(other);
}

#define IMPLEMENT_DEVICEOBJECT_GETDEVICE(DerivedType) \
template <> \
Device<DeviceObject<DerivedType>::GetApi()>& DeviceObject<DerivedType>::GetDevice(DeviceHandle<GetApi()> deviceHandle) const noexcept \
{ \
	if (auto app = std::static_pointer_cast<RHIApplication>(core::Application::Get())) \
		return app->GetRHI<GetApi()>().GetDevice( \
			deviceHandle ? deviceHandle : SuperType::GetDesc().device); \
	static Device<GetApi()> gNullDevice{}; \
	return gNullDevice; \
}

} // namespace rhi
