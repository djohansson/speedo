#pragma once

#include "file.h"
#include "instance.h"
#include "types.h"
#include "utils.h"

#include <atomic>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>

template <GraphicsBackend B>
struct SwapchainConfiguration
{
    SurfaceFormat<B> surfaceFormat = {};
	PresentMode<B> presentMode;
	uint8_t imageCount = 0;
};

template <GraphicsBackend B>
struct DeviceConfiguration
{
    uint32_t physicalDeviceIndex = 0;
    std::optional<SwapchainConfiguration<B>> swapchainConfig = {};
    // std::optional<bool> useShaderFloat16;
    // std::optional<bool> useDescriptorIndexing;
    // std::optional<bool> useScalarBlockLayout;
    // std::optional<bool> useHostQueryReset;
    // std::optional<bool> useTimelineSemaphores;
    // std::optional<bool> useBufferDeviceAddress;
};

enum QueueFamilyFlags : uint8_t
{
    Graphics = 1 << 0,
    Compute = 1 << 1,
    Transfer = 1 << 2,
    Sparse = 1 << 3,
    Present = 1 << 7,
};

template <GraphicsBackend B>
struct QueueFamilyDesc
{
    std::vector<QueueHandle<B>> queues;
    std::vector<CommandPoolHandle<B>> commandPools; // (frameCount:1) * queueCount
    uint8_t flags = 0;
};

template <GraphicsBackend B>
class DeviceContext : public Noncopyable
{
public:

	DeviceContext(
        const std::shared_ptr<InstanceContext<B>>& instanceContext,
        AutoSaveJSONFileObject<DeviceConfiguration<B>>&& config);
    ~DeviceContext();

    const auto& getDesc() const { return myConfig; }
    auto getDevice() const { return myDevice; }
    auto getPhysicalDevice() const { return myInstance->getPhysicalDevices()[myConfig.physicalDeviceIndex]; }
    const auto& getPhysicalDeviceInfo() const { return myInstance->getPhysicalDeviceInfo(getPhysicalDevice()); }
    auto getSurface() const { return myInstance->getSurface(); }

    const auto& getQueueFamilies() const { return myQueueFamilyDescs; }

    uint32_t getGraphicsQueueFamilyIndex() const { return myGraphicsQueueFamilyIndex; }
    uint32_t getTransferQueueFamilyIndex() const { return myTransferQueueFamilyIndex; }
    uint32_t getComputeQueueFamilyIndex() const { return myComputeQueueFamilyIndex; }

    auto getGraphicsQueue() const { return getQueueFamilies()[getGraphicsQueueFamilyIndex()].queues[myCurrentGraphicsQueue]; }
    auto getTransferQueue() const { return getQueueFamilies()[getTransferQueueFamilyIndex()].queues[myCurrentTransferQueue]; }
    auto getComputeQueue() const { return getQueueFamilies()[getComputeQueueFamilyIndex()].queues[myCurrentComputeQueue]; }

    // temp
    const auto& getGraphicsCommandPools() const { return getQueueFamilies()[getGraphicsQueueFamilyIndex()].commandPools; }
    const auto& getTransferCommandPools() const { return getQueueFamilies()[getTransferQueueFamilyIndex()].commandPools; }
    const auto& getComputeCommandPools() const { return getQueueFamilies()[getComputeQueueFamilyIndex()].commandPools; }
    //

    auto getAllocator() const { return myAllocator; }
    auto getDescriptorPool() const { return myDescriptorPool; }

    const auto& getTimelineSemaphore() const { return myTimelineSemaphore; }
    uint64_t getTimelineSemaphoreValue() const;

    auto& timelineValue() { return myTimelineValue; }
    bool hasReached(uint64_t timelineValue) const { return timelineValue <= getTimelineSemaphoreValue(); }
    void wait(uint64_t timelineValue) const;
    void addTimelineCallback(std::function<void(uint64_t)>&& callback);
    void addTimelineCallback(uint64_t timelineValue, std::function<void(uint64_t)>&& callback);
    void addTimelineCallbacks(
        uint64_t timelineValue,
        const std::list<std::function<void(uint64_t)>>& callbacks);
    void processTimelineCallbacks(std::optional<uint64_t> timelineValue = std::nullopt);

    void addOwnedObject(uint32_t ownerId, ObjectType<B> objectType, uint64_t objectHandle, const char* objectName);
    void clearOwnedObjects(uint32_t ownerId);

    uint32_t getTypeCount(ObjectType<B> type);

private:

    std::shared_ptr<InstanceContext<B>> myInstance;
    AutoSaveJSONFileObject<DeviceConfiguration<B>> myConfig;
    DeviceHandle<B> myDevice = 0;

    std::vector<QueueFamilyDesc<B>> myQueueFamilyDescs;
    uint32_t myGraphicsQueueFamilyIndex = 0; // todo: configure
    uint32_t myTransferQueueFamilyIndex = 1; // todo: configure
    uint32_t myComputeQueueFamilyIndex = 2; // todo: configure
    uint32_t myCurrentGraphicsQueue = 0; // todo:
    uint32_t myCurrentTransferQueue = 0; // todo:
    uint32_t myCurrentComputeQueue = 0; // todo:
    
    AllocatorHandle<B> myAllocator = 0;
	DescriptorPoolHandle<B> myDescriptorPool = 0;

    SemaphoreHandle<B> myTimelineSemaphore = 0;
    std::atomic_uint64_t myTimelineValue = 0;

    // todo: make to an atomic queue to avoid excessive locking
    std::recursive_mutex myTimelineCallbacksMutex;
    std::list<std::pair<uint64_t, std::function<void(uint64_t)>>> myTimelineCallbacks;

    struct Object
    {
        ObjectType<B> type = {};
        uint64_t handle = 0;
        std::string name;
    };

    std::shared_mutex myObjectsMutex; // todo: replace with asserting mutex
    std::map<uint32_t, std::vector<Object>> myOwnerToDeviceObjectsMap;
    std::map<ObjectType<B>, uint32_t> myObjectTypeToCountMap;
};

template <GraphicsBackend B>
struct DeviceResourceCreateDesc
{
    std::string name;
};

template <GraphicsBackend B>
class DeviceResource : public Noncopyable
{
public:

    virtual ~DeviceResource();

    const auto& getName() const { return myName; }
    const auto& getId() const { return myId; }

protected:

    DeviceResource(DeviceResource<B>&& other);
    DeviceResource( // no object names are set
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        const DeviceResourceCreateDesc<B>& desc);
    DeviceResource( // uses desc.name and one objectType for all objectHandles
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        const DeviceResourceCreateDesc<B>& desc,
        uint32_t objectCount,
        ObjectType<B> objectType,
        const uint64_t* objectHandles);

    DeviceResource& operator=(DeviceResource&& other);

    const auto& getDeviceContext() const { return myDevice; }

private:

    std::shared_ptr<DeviceContext<B>> myDevice;
    std::string myName;
    uint32_t myId = 0;
    static uint32_t sId;
};

#include "device.inl"
