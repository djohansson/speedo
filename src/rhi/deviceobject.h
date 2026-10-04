#pragma once

#include <vector>

#include <utility>

#include <core/uuids_extra.h>
#include <rhi/capi.h>
#include <rhi/object.h>

#include <cstdint>
#include <string_view>

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
				.uuid = uuids::NewUuid(),
				.instance = SuperType::GetInstance(),
				.name = std::string(name),
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

// validation builds track the graphics api objects that exist, by type and handle: call TrackObject where an object
// is created and UntrackObject where it is destroyed. GetTypeCount reports the number of live objects of a type.
// TrackObject names the object for debugging, through its device: instance level objects created before there is a
// device (the instance, physical devices, surfaces) are tracked with TrackInstanceObject instead, and named by
// NameInstanceObjects once a device exists. handles must not be null, and names not empty. other builds do nothing.
#if (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
template <GraphicsApi G>
void TrackObject(DeviceHandle<G> device, ObjectType<G> type, uint64_t handle, std::string_view name);
template <GraphicsApi G>
void TrackInstanceObject(ObjectType<G> type, uint64_t handle, std::string_view name);
template <GraphicsApi G>
void NameInstanceObjects(DeviceHandle<G> device);
template <GraphicsApi G>
void UntrackObject(ObjectType<G> type, uint64_t handle);
template <GraphicsApi G>
[[nodiscard]] uint32_t GetTypeCount(ObjectType<G> type);
// the live objects of each tracked type, by name
template <GraphicsApi G>
[[nodiscard]] std::vector<std::pair<std::string_view, uint32_t>> GetObjectCounts();
#else
template <GraphicsApi G>
void TrackObject(DeviceHandle<G> /*device*/, ObjectType<G> /*type*/, uint64_t /*handle*/, std::string_view /*name*/) {}
template <GraphicsApi G>
void TrackInstanceObject(ObjectType<G> /*type*/, uint64_t /*handle*/, std::string_view /*name*/) {}
template <GraphicsApi G>
void NameInstanceObjects(DeviceHandle<G> /*device*/) {}
template <GraphicsApi G>
void UntrackObject(ObjectType<G> /*type*/, uint64_t /*handle*/) {}
template <GraphicsApi G>
[[nodiscard]] uint32_t GetTypeCount(ObjectType<G> /*type*/) { return 0; }
template <GraphicsApi G>
[[nodiscard]] std::vector<std::pair<std::string_view, uint32_t>> GetObjectCounts() { return {}; }
#endif

} // namespace rhi

#include <rhi/deviceobject.inl>
