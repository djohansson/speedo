#pragma once

#include <core/utils.h>
#include <rhi/object.h>
#include <rhi/instance.h>
#include <rhi/queue.h>
#include <rhi/pipeline.h>

#include <uuid.h>

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

namespace rhi
{

template <GraphicsApi G>
class Device;

template <GraphicsApi G>
struct DeviceCreateDesc final : ObjectCreateDesc<G>
{
	//uint64_t vendorAndDeviceId = 0ULL; // deviceID & (vendorID << 32)
	PhysicalDeviceHandle<G> physicalDevice{};
};

template <GraphicsApi G>
struct ObjectTraits<Device<G>>
{
	using CreateDescType = DeviceCreateDesc<G>;
};

template <GraphicsApi G>
class Device final : public Object<Device<G>>
{
	using PipelineLayoutHandleMapType = core::UnorderedMap<size_t, PipelineLayoutHandle<G>>;
	// resources are keyed by their own uuid (unique among live objects, see ObjectCreateDesc): hash and compare
	// them by it, and look them up by a plain uuid
	struct ResourceHash
	{
		using is_transparent = void;
		[[nodiscard]] size_t operator()(const uuids::uuid& uuid) const noexcept { return std::hash<uuids::uuid>{}(uuid); }
		[[nodiscard]] size_t operator()(const std::shared_ptr<IObject>& resource) const noexcept { return (*this)(resource->GetUuid()); }
	};
	struct ResourceEqual
	{
		using is_transparent = void;
		[[nodiscard]] static const uuids::uuid& Key(const uuids::uuid& uuid) noexcept { return uuid; }
		[[nodiscard]] static const uuids::uuid& Key(const std::shared_ptr<IObject>& resource) noexcept { return resource->GetUuid(); }
		[[nodiscard]] bool operator()(const auto& lhs, const auto& rhs) const noexcept { return Key(lhs) == Key(rhs); }
	};
	using ResourceSetType = core::UnorderedSet<std::shared_ptr<IObject>, ResourceHash, ResourceEqual>;

public:
	using SuperType = Object<Device<G>>;
	using CreateDescType = ObjectTraits<Device<G>>::CreateDescType;
	
	constexpr Device() noexcept = default;
	explicit Device(CreateDescType&& desc, const Instance<G>& instance);
	Device(const Device&) = delete;
	Device(Device&& other) noexcept;
	~Device();

	[[nodiscard]] Device& operator=(const Device&) = delete;
	[[nodiscard]] Device& operator=(Device&& other) noexcept;

	void Swap(Device& other) noexcept;
	friend void Swap(Device& lhs, Device& rhs) noexcept { lhs.Swap(rhs); }

	[[nodiscard]] operator auto() const noexcept { return myDevice; }//NOLINT(google-explicit-constructor)

	[[nodiscard]] auto GetAllocator() const noexcept { return myAllocator; }

	[[nodiscard]] PhysicalDeviceHandle<G> GetPhysicalDevice() const noexcept { return SuperType::GetDesc().physicalDevice; }

	[[nodiscard]] const auto& GetQueueFamilies() const noexcept { return myQueueFamilyDescs; }
	
	[[nodiscard]] auto& GetQueues() noexcept { return myQueues; }
	[[nodiscard]] const auto& GetQueues() const noexcept { return myQueues; }
	// note: queue types without a dedicated queue alias another type's context (see InternalCreateQueues), so never
	// hold locks on more than one queue type at once unless you know they are distinct, or it will self-deadlock.
	[[nodiscard]] QueueTimelineContext<G>& GetQueue(QueueType type) const { return *myQueues.at(type); }

	[[nodiscard]] auto GetPipelineLayoutHandle(size_t nameHash) const { return myPipelineLayoutHandles.at(nameHash); }
	[[nodiscard]] auto GetPipelineLayoutHandle(std::string_view name) const { return myPipelineLayoutHandles.at(std::hash<std::string_view>{}(name)); }
	[[nodiscard]] auto& GetPipelineLayoutHandles() noexcept { return myPipelineLayoutHandles; }
	[[nodiscard]] const auto& GetPipelineLayoutHandles() const noexcept { return myPipelineLayoutHandles; }
	
	[[nodiscard]] auto& GetPipeline() noexcept { return myPipeline; }
	[[nodiscard]] const auto& GetPipeline() const noexcept { return myPipeline; }

	// resources owned by the device, keyed by their uuids. callers keep the uuid CreateResource returns to find one.
	[[nodiscard]] bool HasResource(const uuids::uuid& uuid) const { return myResources.contains(uuid); }
	template <typename T>
	[[nodiscard]] std::shared_ptr<T> GetResource(const uuids::uuid& uuid) const
	{
		return static_pointer_cast<T>(*InternalGetResourceIterator(uuid));
	}
	// stores a resource created elsewhere (e.g. by a loader), found by its own uuid from now on. the only way into the
	// set: it must not be null, nor already stored.
	void AddResource(std::shared_ptr<IObject> resource)
	{
		ENSUREF(resource, "cannot add a null resource");
		ENSUREF(myResources.insert(std::move(resource)).second, "resource is already stored");
	}
	// constructs a T from args (its create desc first) and stores it. returns the resource: keep its GetUuid() to find
	// it again.
	template <class T, class... Args>
	[[nodiscard]] std::shared_ptr<T> CreateResource(Args&&... args)
	{
		auto resource = std::make_shared<T>(std::forward<Args>(args)...);
		AddResource(resource);
		return resource;
	}
	// stores resource in place of the previous one, given by its uuid or itself, and returns the previous one, so the
	// caller can defer its destruction until the gpu is no longer using it. resource is found by its own uuid from now
	// on. either may be absent: a nil uuid (or null previous) just adds resource, and a null resource just removes the
	// previous one. a previous uuid that is not nil must be stored.
	[[nodiscard]] std::shared_ptr<IObject> ReplaceResource(const std::shared_ptr<IObject>& previous, std::shared_ptr<IObject> resource)
	{
		return ReplaceResource(previous ? previous->GetUuid() : uuids::uuid{}, std::move(resource));
	}
	[[nodiscard]] std::shared_ptr<IObject> ReplaceResource(const uuids::uuid& previousUuid, std::shared_ptr<IObject> resource)
	{
		std::shared_ptr<IObject> previous;
		if (!previousUuid.is_nil())
		{
			auto previousIt = InternalGetResourceIterator(previousUuid);
			previous = *previousIt;
			myResources.erase(previousIt);
		}
		if (resource)
			AddResource(std::move(resource));
		return previous;
	}
	void EraseResource(const uuids::uuid& uuid) { myResources.erase(uuid); }

	[[nodiscard]] DeviceObjectCreateDesc<G> CreateDeviceObjectCreateDesc(std::string_view name = {}) const noexcept
	{
		return DeviceObjectCreateDesc<G>{
			ObjectCreateDesc<G>{
				.uuid = uuids::NewUuid(),
				.instance = SuperType::GetDesc().instance,
				.name = std::string(name),
			},
			myDevice
		};
	}

	[[nodiscard]] bool SupportsFeature(StructureType<G> feature, const Instance<G>& instance) const;

	void WaitIdle() const;

private:
	// queues and pipeline are created by RHI once the device is registered in RHI::myDevices,
	// since DeviceObject<T>::GetDevice() resolves devices through RHIApplication::GetRHI<G>().
	template <GraphicsApi> friend class RHI;

	void InternalCreateQueues();
	void InternalCreatePipeline();
	[[nodiscard]] auto InternalGetResourceIterator(const uuids::uuid& uuid) const
	{
		auto resourceit = myResources.find(uuid);
		ENSUREF(resourceit != myResources.end(), "no resource with uuid {}", uuids::to_string(uuid));
		return resourceit;
	}

	DeviceHandle<G> myDevice{};
	AllocatorHandle<G> myAllocator{};//NOLINT(google-readability-casting)
	std::vector<QueueFamilyDesc<G>> myQueueFamilyDescs;
	// shared_ptr so that aliased queue types share the same ConcurrentAccess (and thus the same mutex), not just the data
	core::UnorderedMap<QueueType, std::shared_ptr<QueueTimelineContext<G>>> myQueues;
	Pipeline<G> myPipeline;
	PipelineLayoutHandleMapType myPipelineLayoutHandles;
	ResourceSetType myResources;
};

} // namespace rhi
