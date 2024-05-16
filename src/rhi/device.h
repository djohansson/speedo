#pragma once

#include "instance.h"
#include "types.h"

#include <core/file.h>
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
	consteval std::string_view GetName() const { return "device"; }

	uint32_t physicalDeviceIndex = 0ul; // todo: replace with deviceID
	// std::optional<bool> useShaderFloat16;
	// std::optional<bool> useDescriptorIndexing;
	// std::optional<bool> useScalarBlockLayout;
	// std::optional<bool> useHostQueryReset;
	// std::optional<bool> useTimelineSemaphores;
	// std::optional<bool> useBufferDeviceAddress;
};

template <GraphicsApi G>
struct QueueFamilyDesc
{
	uint32_t queueCount = 0ul;
	uint32_t flags = 0ul;
};

template <GraphicsApi G>
class Device final : public Noncopyable, public Nonmovable
{
public:
	Device(
		const std::shared_ptr<Instance<G>>& instance,
		DeviceConfiguration<G>&& defaultConfig = {});
	~Device();

	operator auto() const noexcept { return myDevice; }

	auto GetInstance() const noexcept { return myInstance; } // todo: make global?
	const auto& GetConfig() const noexcept { return myConfig; }
	auto GetPhysicalDevice() const noexcept
	{
		return myInstance->GetPhysicalDevices()[myPhysicalDeviceIndex];
	}
	const auto& GetPhysicalDeviceInfo() const noexcept
	{
		return myInstance->GetPhysicalDeviceInfo(GetPhysicalDevice());
	}

	const auto& GetQueueFamilies() const noexcept { return myQueueFamilyDescs; }

	auto GetAllocator() const noexcept { return myAllocator; }

	auto& TimelineValue() { return myTimelineValue; }

	void WaitIdle() const;

#if (GRAPHICS_VALIDATION_LEVEL > 0)
	void AddOwnedObjectHandle(
		const uuids::uuid& ownerId,
		ObjectType<G> objectType,
		uint64_t objectHandle,
		std::string&& objectName);
	void EraseOwnedObjectHandle(const uuids::uuid& ownerId, uint64_t objectHandle);
	void ClearOwnedObjectHandles(const uuids::uuid& ownerId);
	uint32_t GetTypeCount(ObjectType<G> type);
#endif

private:
	std::shared_ptr<Instance<G>> myInstance;
	file::Object<DeviceConfiguration<G>, file::AccessMode::kReadWrite, true> myConfig;
	DeviceHandle<G> myDevice{};
	uint32_t myPhysicalDeviceIndex = 0ul;
	std::vector<QueueFamilyDesc<G>> myQueueFamilyDescs;
	AllocatorHandle<G> myAllocator{};
	std::atomic_uint64_t myTimelineValue;

#if (GRAPHICS_VALIDATION_LEVEL > 0)
	struct ObjectNameInfo : ObjectInfo<G>
	{
		std::string name;
	};
	using ObjectInfos = std::vector<ObjectNameInfo>;

	std::shared_mutex
		myObjectMutex; // protects myOwnerToDeviceObjectInfoMap & myObjectTypeToCountMap
	UnorderedMap<uint64_t, ObjectInfos, IdentityHash<uint64_t>> myOwnerToDeviceObjectInfoMap;
	UnorderedMap<ObjectType<G>, uint32_t> myObjectTypeToCountMap;
#endif
};

struct DeviceObjectCreateDesc
{
	std::string name;
};

template <GraphicsApi G>
class DeviceObject : public Noncopyable
{
public:
	virtual ~DeviceObject();

	DeviceObject& operator=(DeviceObject&& other) noexcept;
	void Swap(DeviceObject& rhs) noexcept;

	std::string_view GetName() const noexcept { return myDesc.name; }
	const uuids::uuid& GetUid() const noexcept { return myUid; }
	bool IsValid() const noexcept { return !myUid.is_nil(); }

protected:
	constexpr DeviceObject() noexcept = default;
	DeviceObject(DeviceObject<G>&& other) noexcept;
	DeviceObject( // no object names are set
		const std::shared_ptr<Device<G>>& device,
		DeviceObjectCreateDesc&& desc);
	DeviceObject( // uses desc.name and one objectType for all objectHandles
		const std::shared_ptr<Device<G>>& device,
		DeviceObjectCreateDesc&& desc,
		uint32_t objectCount,
		ObjectType<G> objectType,
		const uint64_t* objectHandles);

	const auto& GetDevice() const noexcept { return myDevice; }

private:
	std::shared_ptr<Device<G>> myDevice;
	DeviceObjectCreateDesc myDesc{};
	uuids::uuid myUid{};
};
