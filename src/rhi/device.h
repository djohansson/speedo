#pragma once

#include "instance.h"
#include "types.h"

#include <core/file.h>
#include <core/resource.h>
#include <core/utils.h>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

#include <stduuid/uuid.h>

template <GraphicsApi G>
struct DeviceConfiguration
{
	[[nodiscard]] consteval std::string_view GetName() const { return "device"; }

	uint32_t physicalDeviceIndex = 0UL; // todo: replace with deviceID
};

template <GraphicsApi G>
struct QueueFamilyDesc
{
	uint32_t queueCount = 0UL;
	uint32_t flags = 0UL;
};

template <GraphicsApi G>
class Device final
{
public:
	explicit Device(
		const std::shared_ptr<Instance<G>>& instance,
		DeviceConfiguration<G>&& defaultConfig = {});
	Device(const Device&) = delete;
	Device(Device&& other) noexcept = delete;
	~Device();

	[[nodiscard]] Device& operator=(const Device&) = delete;
	[[nodiscard]] Device& operator=(Device&& other) noexcept = delete;

	[[nodiscard]] operator auto() const noexcept { return myDevice; }//NOLINT(google-explicit-constructor)

	[[nodiscard]] const auto& GetInstance() const noexcept { return myInstance; } // todo: make global?
	[[nodiscard]] const auto& GetConfig() const noexcept { return myConfig; }
	[[nodiscard]] auto GetPhysicalDevice() const noexcept
	{
		return myInstance->GetPhysicalDevices()[myPhysicalDeviceIndex];
	}
	[[nodiscard]] const auto& GetPhysicalDeviceInfo() const noexcept
	{
		return myInstance->GetPhysicalDeviceInfo(GetPhysicalDevice());
	}

	[[nodiscard]] const auto& GetQueueFamilies() const noexcept { return myQueueFamilyDescs; }

	[[nodiscard]] auto GetAllocator() const noexcept { return myAllocator; }

	void WaitIdle() const;

#if (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
	void AddOwnedObjectHandle(
		const uuids::uuid& ownerId,
		ObjectType<G> objectType,
		uint64_t objectHandle,
		std::string&& objectName);
	void EraseOwnedObjectHandle(const uuids::uuid& ownerId, uint64_t objectHandle);
	void ClearOwnedObjectHandles(const uuids::uuid& ownerId);
	[[nodiscard]] uint32_t GetTypeCount(ObjectType<G> type);
#endif

private:
	std::shared_ptr<Instance<G>> myInstance;
	file::Object<DeviceConfiguration<G>, file::AccessMode::kReadWrite, true> myConfig;
	DeviceHandle<G> myDevice{};
	uint32_t myPhysicalDeviceIndex = 0UL;
	std::vector<QueueFamilyDesc<G>> myQueueFamilyDescs;
	AllocatorHandle<G> myAllocator{};//NOLINT(google-readability-casting)

#if (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
	struct ObjectNameInfo : ObjectInfo<G>
	{
		std::string name;
	};
	using ObjectInfos = std::vector<ObjectNameInfo>;

	// protects myOwnerToDeviceObjectInfoMap & myObjectTypeToCountMap
	std::shared_mutex myObjectMutex;
	UnorderedMap<uint64_t, ObjectInfos, IdentityHash<uint64_t>> myOwnerToDeviceObjectInfoMap;
	UnorderedMap<ObjectType<G>, uint32_t> myObjectTypeToCountMap;
#endif
};

struct DeviceObjectCreateDesc
{
	std::string name;
};

template <GraphicsApi G>
class DeviceObject : public IResource
{
public:
	DeviceObject(const DeviceObject&) = delete;
	virtual ~DeviceObject();

	[[nodiscard]] const uuids::uuid& GetUuid() const noexcept final { return myUuid; }

	[[nodiscard]] DeviceObject& operator=(const DeviceObject&) = delete;

	void Swap(DeviceObject& rhs) noexcept;

	[[nodiscard]] std::string_view GetName() const noexcept { return myDesc.name; }
	[[nodiscard]] bool IsValid() const noexcept { return !myUuid.is_nil(); }

protected:
	constexpr DeviceObject() noexcept = default;
	DeviceObject(DeviceObject<G>&& other) noexcept;
	DeviceObject( // no object names are set
		const std::shared_ptr<Device<G>>& device,
		DeviceObjectCreateDesc&& desc,
		uuids::uuid&& uuid);
	DeviceObject( // uses desc.name and one objectType for all objectHandles
		const std::shared_ptr<Device<G>>& device,
		DeviceObjectCreateDesc&& desc,
		uint32_t objectCount,
		ObjectType<G> objectType,
		const uint64_t* objectHandles,
		uuids::uuid&& uuid);

	[[maybe_unused]] DeviceObject& operator=(DeviceObject&& other) noexcept;

	[[nodiscard]] const auto& InternalGetDevice() const noexcept { return myDevice; }

private:
	std::shared_ptr<Device<G>> myDevice;
	DeviceObjectCreateDesc myDesc{};
	uuids::uuid myUuid;
};
