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
	using ResourceMapType = core::UnorderedMap<uuids::uuid, std::shared_ptr<IObject>>;

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

	[[nodiscard]] bool HasResource(const uuids::uuid& uuid) const { return myResources.contains(uuid); }
	[[nodiscard]] bool HasResource(std::string_view name) const { return myResources.contains(uuids::uuid_name_generator{uuids::uuid_namespace_oid}(name)); }
	template <typename T>
	[[nodiscard]] auto GetResource(const uuids::uuid& uuid) const { return static_pointer_cast<T>(myResources.at(uuid)); }
	template <typename T>
	[[nodiscard]] auto GetResource(std::string_view name) const { return static_pointer_cast<T>(myResources.at(uuids::uuid_name_generator{uuids::uuid_namespace_oid}(name))); }
	template <class T, class... Args>
	[[nodiscard]] auto CreateResource(uuids::uuid&& uuid, Args&&... args)
	{
		auto resource = std::make_shared<T>(std::forward<Args>(args)...);
		auto [it, inserted] = myResources.emplace(std::forward<uuids::uuid>(uuid), resource);
		return std::make_tuple(it->first, resource, inserted);
	}
	template <class T, class... Args>
	[[nodiscard]] auto CreateResource(std::string_view name, Args&&... args)
	{
		return CreateResource<T>(uuids::uuid_name_generator{uuids::uuid_namespace_oid}(name), std::forward<Args>(args)...);
	}
	// inserts or replaces a resource. returns the previous one (if any), so the caller can defer its destruction
	// until the gpu is no longer using it.
	[[nodiscard]] std::shared_ptr<IObject> ReplaceResource(const uuids::uuid& uuid, std::shared_ptr<IObject> resource)
	{
		return std::exchange(myResources[uuid], std::move(resource));
	}
	template <typename T>
	[[maybe_unused]] auto ExtractResource(const uuids::uuid& uuid) { return static_pointer_cast<T>(myResources.extract(uuid)); }
	void EraseResource(const uuids::uuid& uuid) { myResources.erase(uuid); }

	[[nodiscard]] DeviceObjectCreateDesc<G> CreateDeviceObjectCreateDesc(std::string_view name = {}) const noexcept
	{
		return DeviceObjectCreateDesc<G>{
			ObjectCreateDesc<G>{
				.uuid = uuids::uuid_name_generator{uuids::uuid_namespace_oid}(name),
				.instance = SuperType::GetDesc().instance,
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

	DeviceHandle<G> myDevice{};
	AllocatorHandle<G> myAllocator{};//NOLINT(google-readability-casting)
	std::vector<QueueFamilyDesc<G>> myQueueFamilyDescs;
	// shared_ptr so that aliased queue types share the same ConcurrentAccess (and thus the same mutex), not just the data
	core::UnorderedMap<QueueType, std::shared_ptr<QueueTimelineContext<G>>> myQueues;
	Pipeline<G> myPipeline;
	PipelineLayoutHandleMapType myPipelineLayoutHandles;
	ResourceMapType myResources;
};

} // namespace rhi
