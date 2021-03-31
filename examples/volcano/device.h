#pragma once

#include "concurrency-utils.h"
#include "file.h"
#include "instance.h"
#include "types.h"
#include "utils.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <uuid.h>

template <GraphicsBackend B>
struct SwapchainConfiguration
{
    SurfaceFormat<B> surfaceFormat = {};
	PresentMode<B> presentMode = static_cast<PresentMode<B>>(0);
	uint8_t imageCount = 0;
};

template <GraphicsBackend B>
struct DeviceConfiguration
{
    uint32_t physicalDeviceIndex = 0ul;
    std::optional<SwapchainConfiguration<B>> swapchainConfig = {};
    // std::optional<bool> useShaderFloat16;
    // std::optional<bool> useDescriptorIndexing;
    // std::optional<bool> useScalarBlockLayout;
    // std::optional<bool> useHostQueryReset;
    // std::optional<bool> useTimelineSemaphores;
    // std::optional<bool> useBufferDeviceAddress;
};

enum QueueFamilyFlagBits
{
    QueueFamilyFlagBits_Graphics = 1 << 0,
    QueueFamilyFlagBits_Compute = 1 << 1,
    QueueFamilyFlagBits_Transfer = 1 << 2,
    QueueFamilyFlagBits_Sparse = 1 << 3,
    QueueFamilyFlagBits_AllExceptPresent = (1 << 4) - 1,
    QueueFamilyFlagBits_Present = 1 << 4,
    QueueFamilyFlagBits_All = (1 << 5) - 1,
};

template <GraphicsBackend B>
struct QueueFamilyDesc
{
    uint32_t queueCount = 0ul;
    uint32_t flags = 0ul;
};

using TimelineCallback = std::tuple<uint64_t, std::function<void(uint64_t)>>;
using TimelineCallbackQueue = ConcurrentDeque<TimelineCallback>;

template <GraphicsBackend B>
class DeviceContext : public Noncopyable
{
public:

	DeviceContext(
        const std::shared_ptr<InstanceContext<B>>& instanceContext,
        AutoSaveJSONFileObject<DeviceConfiguration<B>>&& config);
    ~DeviceContext();

    const auto& getDesc() const noexcept { return myConfig; }
    auto getDevice() const noexcept { return myDevice; }
    auto getPhysicalDevice() const noexcept { return myInstance->getPhysicalDevices()[myConfig.physicalDeviceIndex]; }
    const auto& getPhysicalDeviceInfo() const noexcept { return myInstance->getPhysicalDeviceInfo(getPhysicalDevice()); }
    auto getSurface() const noexcept { return myInstance->getSurface(); } // todo: remove

    const auto& getQueueFamilies() const noexcept { return myQueueFamilyDescs; }

    auto getAllocator() const noexcept { return myAllocator; }

    const auto& getTimelineSemaphore() const noexcept { return myTimelineSemaphore; }
    uint64_t getTimelineSemaphoreValue() const;

    auto& timelineValue() { return myTimelineValue; }
    bool hasReached(uint64_t timelineValue) const { return timelineValue <= getTimelineSemaphoreValue(); }
    void wait(uint64_t timelineValue) const;
    void waitIdle() const;
    uint64_t addTimelineCallback(std::function<void(uint64_t)>&& callback);
    uint64_t addTimelineCallback(TimelineCallback&& callback);
    bool processTimelineCallbacks(std::optional<uint64_t> timelineValue = std::nullopt);

    void addOwnedObjectHandle(const uuids::uuid& ownerId, ObjectType<B> objectType, uint64_t objectHandle, const char* objectName);
    void eraseOwnedObjectHandle(const uuids::uuid& ownerId, uint64_t objectHandle);
    void clearOwnedObjectHandles(const uuids::uuid& ownerId);

    uint32_t getTypeCount(ObjectType<B> type);

private:

    std::shared_ptr<InstanceContext<B>> myInstance;
    AutoSaveJSONFileObject<DeviceConfiguration<B>> myConfig;
    DeviceHandle<B> myDevice = {};

    std::vector<QueueFamilyDesc<B>> myQueueFamilyDescs;
    
    AllocatorHandle<B> myAllocator = {};

    SemaphoreHandle<B> myTimelineSemaphore = {};
    std::atomic_uint64_t myTimelineValue = {};

    TimelineCallbackQueue myTimelineCallbacks;

    struct ObjectNameInfo : ObjectInfo<B>
    {
        std::string name;
    };

    using ObjectInfos = std::vector<ObjectNameInfo>;
    UpgradableSharedMutex<> myObjectMutex; // protects myOwnerToDeviceObjectInfoMap & myObjectTypeToCountMap
    UnorderedMap<uint64_t, ObjectInfos, IdentityHash<uint64_t>> myOwnerToDeviceObjectInfoMap;
    UnorderedMap<ObjectType<B>, uint32_t> myObjectTypeToCountMap;
};

struct DeviceObjectCreateDesc
{
    std::string name;
};

template <GraphicsBackend B>
class DeviceObject : public Noncopyable
{
public:

    virtual ~DeviceObject();

    const auto& getName() const noexcept { return myDesc.name; }
    const uuids::uuid& getUid() const noexcept { return myUid; }
    bool isValid() const noexcept { return !myUid.is_nil(); }

protected:

    constexpr DeviceObject() noexcept = default;
    DeviceObject(DeviceObject<B>&& other) noexcept;
    DeviceObject( // no object names are set
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        DeviceObjectCreateDesc&& desc);
    DeviceObject( // uses desc.name and one objectType for all objectHandles
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        DeviceObjectCreateDesc&& desc,
        uint32_t objectCount,
        ObjectType<B> objectType,
        const uint64_t* objectHandles);

    DeviceObject& operator=(DeviceObject&& other) noexcept;

    void swap(DeviceObject& rhs) noexcept;

    const auto& getDeviceContext() const noexcept { return myDevice; }

private:

    std::shared_ptr<DeviceContext<B>> myDevice;
    DeviceObjectCreateDesc myDesc = {};
    uuids::uuid myUid = {};
};

#include "device.inl"
