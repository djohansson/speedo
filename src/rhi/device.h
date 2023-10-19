#pragma once

#include "instance.h"
#include "types.h"

#include <core/concurrency-utils.h>
#include <core/file.h>
#include <core/utils.h>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>

#include <stduuid/uuid.h>

template <GraphicsApi G>
struct DeviceConfiguration
{
	constexpr std::string_view getName() const { return "device"; }

	uint32_t physicalDeviceIndex = 0ul; // todo: replace with deviceID
	// std::optional<bool> useShaderFloat16;
	// std::optional<bool> useDescriptorIndexing;
	// std::optional<bool> useScalarBlockLayout;
	// std::optional<bool> useHostQueryReset;
	// std::optional<bool> useTimelineSemaphores;
	// std::optional<bool> useBufferDeviceAddress;

	GLZ_LOCAL_META(DeviceConfiguration<G>, physicalDeviceIndex);
};

enum QueueFamilyFlagBits
{
	QueueFamilyFlagBits_Graphics = 1 << 0,
	QueueFamilyFlagBits_Compute = 1 << 1,
	QueueFamilyFlagBits_Transfer = 1 << 2,
	QueueFamilyFlagBits_Sparse = 1 << 3,
	QueueFamilyFlagBits_All = (1 << 4) - 1,
};

template <GraphicsApi G>
struct QueueFamilyDesc
{
	uint32_t queueCount = 0ul;
	uint32_t flags = 0ul;
};

using TimelineCallback = std::tuple<uint64_t, std::function<void(uint64_t)>>;

template <GraphicsApi G>
class Device final : public Noncopyable, public Nonmovable
{
public:
	Device(
		const std::shared_ptr<Instance<G>>& instance,
		DeviceConfiguration<G>&& defaultConfig = {});
	~Device();

	operator auto() const noexcept { return myDevice; }

	auto getInstance() const noexcept { return myInstance; } // todo: make global?
	const auto& getConfig() const noexcept { return myConfig; }
	auto getPhysicalDevice() const noexcept
	{
		return myInstance->getPhysicalDevices()[myPhysicalDeviceIndex];
	}
	const auto& getPhysicalDeviceInfo() const noexcept
	{
		return myInstance->getPhysicalDeviceInfo(getPhysicalDevice());
	}

	const auto& getQueueFamilies() const noexcept { return myQueueFamilyDescs; }

	auto getAllocator() const noexcept { return myAllocator; }

	const auto& getTimelineSemaphore() const noexcept { return myTimelineSemaphore; }
	uint64_t getTimelineSemaphoreValue() const;

	auto& timelineValue() { return myTimelineValue; }
	bool hasReached(uint64_t timelineValue) const
	{
		return timelineValue <= getTimelineSemaphoreValue();
	}
	void wait(uint64_t timelineValue) const;
	void waitIdle() const;
	uint64_t addTimelineCallback(std::function<void(uint64_t)>&& callback);
	uint64_t addTimelineCallback(TimelineCallback&& callback);
	bool processTimelineCallbacks(uint64_t timelineValue);

#if GRAPHICS_VALIDATION_ENABLED
	void addOwnedObjectHandle(
		const uuids::uuid& ownerId,
		ObjectType<G> objectType,
		uint64_t objectHandle,
		const char* objectName);
	void eraseOwnedObjectHandle(const uuids::uuid& ownerId, uint64_t objectHandle);
	void clearOwnedObjectHandles(const uuids::uuid& ownerId);
	uint32_t getTypeCount(ObjectType<G> type);
#endif

private:
	std::shared_ptr<Instance<G>> myInstance;
	file::Object<DeviceConfiguration<G>, file::AccessMode::ReadWrite, true> myConfig;
	DeviceHandle<G> myDevice{};
	uint32_t myPhysicalDeviceIndex = 0ul;

	std::vector<QueueFamilyDesc<G>> myQueueFamilyDescs;

	AllocatorHandle<G> myAllocator{};

	SemaphoreHandle<G> myTimelineSemaphore{};
	std::atomic_uint64_t myTimelineValue;

	moodycamel::ConcurrentQueue<TimelineCallback> myTimelineCallbacks;

#if GRAPHICS_VALIDATION_ENABLED
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

	std::string_view getName() const noexcept { return myDesc.name; }
	const uuids::uuid& getUid() const noexcept { return myUid; }
	bool isValid() const noexcept { return !myUid.is_nil(); }

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

	DeviceObject& operator=(DeviceObject&& other) noexcept;

	void swap(DeviceObject& rhs) noexcept;

	const auto& getDevice() const noexcept { return myDevice; }

private:
	std::shared_ptr<Device<G>> myDevice;
	DeviceObjectCreateDesc myDesc{};
	uuids::uuid myUid{};
};
