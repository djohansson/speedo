#pragma once

#include <rhi/capi.h>
#include <rhi/object.h>
#include <uuid.h>

#if (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
#include <vector>
#endif

namespace rhi
{

template <GraphicsApi G>
class Device;

template <GraphicsApi G>
struct DeviceObjectCreateDesc : ObjectCreateDesc<G>
{
	DeviceHandle<G> device{};

	// explicit serialize() needed because zpp::bits reflects via structured bindings, which cannot
	// decompose a type whose non-static members are split across itself and a base class. "device" is
	// a runtime-only handle (not meaningful across a save/load round-trip) and is deliberately excluded.
	constexpr static auto serialize(auto& archive, auto& self)//NOLINT(readability-identifier-naming)
	{
		using SelfType = std::remove_reference_t<decltype(self)>;
		using BaseType = std::conditional_t<std::is_const_v<SelfType>, const ObjectCreateDesc<G>, ObjectCreateDesc<G>>;
		return archive(static_cast<BaseType&>(self));
	}

#if (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
	ObjectType<G> objectType{};
	std::vector<const uint64_t*> objectPointers;
#endif
};

template <typename DerivedType>
class DeviceObject : public Object<DerivedType>
{
public:
	using SuperType = Object<DerivedType>;
	using CreateDescType = ObjectTraits<DerivedType>::CreateDescType;

	[[nodiscard]] static consteval GraphicsApi GetApi() { return SuperType::GetApi(); }

	DeviceObject(const DeviceObject&) = delete;
	~DeviceObject() override;
	[[nodiscard]] DeviceObject& operator=(const DeviceObject&) = delete;

	[[nodiscard]] Device<GetApi()>& GetDevice(DeviceHandle<GetApi()> deviceHandle = {}) const noexcept;

	[[nodiscard]] auto CreateDeviceObjectCreateDesc(
		std::string_view name = {},
		DeviceHandle<GetApi()> deviceHandle = {}) const noexcept
	{
		return DeviceObjectCreateDesc<GetApi()>{
			ObjectCreateDesc<GetApi()>{
				.uuid = uuids::uuid_name_generator{uuids::uuid_namespace_oid}(name),
				.instance = SuperType::GetInstance(),
			},
			GetDevice(deviceHandle)
		};
	}

	void Swap(DeviceObject& other) noexcept;

protected:
	constexpr DeviceObject() noexcept = default;
	DeviceObject(DeviceObject&& other) noexcept;
	explicit DeviceObject(CreateDescType&& desc);

	[[maybe_unused]] DeviceObject& operator=(DeviceObject&& other) noexcept;
};

#if (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
inline core::UpgradableSharedMutex gObjectNameInfoMutex;
template <GraphicsApi G>
inline core::UnorderedMap<uint64_t, std::vector<std::pair<ObjectNameInfo<G>, std::unique_ptr<char[]>>>, core::IdentityHash<uint64_t>> gOwnerToObjectNameInfoMap;
template <GraphicsApi G>
inline core::UnorderedMap<ObjectType<G>, uint32_t> gObjectTypeToCountMap;
template <GraphicsApi G>
void AddOwnedObjectHandle(
	DeviceHandle<G> device,
	const uuids::uuid& ownerId,
	ObjectType<G> objectType,
	uint64_t objectHandle,
	std::string_view objectName);
template <GraphicsApi G>
void EraseOwnedObjectHandle(
	const uuids::uuid& ownerId,
	uint64_t objectHandle);
template <GraphicsApi G>
void ClearOwnedObjectHandles(const uuids::uuid& ownerId);
template <GraphicsApi G>
[[nodiscard]] static uint32_t GetTypeCount(ObjectType<G> type);
#endif

} // namespace rhi

#include <rhi/deviceobject.inl>
