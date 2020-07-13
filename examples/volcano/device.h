#pragma once

#include "gfx-types.h"
#include "instance.h"

#include <atomic>
#include <list>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <vector>


template <GraphicsBackend B>
struct SwapchainConfiguration
{
    SurfaceFormat<B> surfaceFormat = {};
	PresentMode<B> presentMode;
	uint8_t imageCount = 0;

    template <class Archive>
    void serialize(Archive& archive)
    {
        archive(
            CEREAL_NVP(surfaceFormat.format),
            CEREAL_NVP(surfaceFormat.colorSpace),
            CEREAL_NVP(presentMode),
            CEREAL_NVP(imageCount)
        );
    }
};

template <GraphicsBackend B>
struct DeviceConfiguration
{
    uint32_t physicalDeviceIndex = 0;
    std::optional<SwapchainConfiguration<B>> swapchainConfiguration = {};
    std::optional<bool> useHostQueryReset;
    std::optional<bool> useTimelineSemaphores;
    std::optional<bool> useScalarBlockLayout;
    std::optional<bool> useShaderFloat16;

    template <class Archive>
    void serialize(Archive& archive)
    {
        archive(
            CEREAL_NVP(physicalDeviceIndex),
            CEREAL_NVP(swapchainConfiguration),
            CEREAL_NVP(useHostQueryReset),
            CEREAL_NVP(useTimelineSemaphores),
            CEREAL_NVP(useShaderFloat16)
        );
    }
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
    std::vector<std::vector<CommandPoolHandle<B>>> commandPools; // frameCount * queueCount
    uint8_t flags = 0;
};

template <GraphicsBackend B>
class DeviceContext
{
public:

    DeviceContext(DeviceContext<B>&& other) = default;
	DeviceContext(
        const std::shared_ptr<InstanceContext<B>>& instanceContext,
        ScopedFileObject<DeviceConfiguration<B>>&& config);
    ~DeviceContext();

    const auto& getDesc() const { return myConfig; }
    const auto& getDevice() const { return myDevice; }
    const auto& getPhysicalDevice() const
    { return myInstance->getPhysicalDevices()[myConfig.physicalDeviceIndex]; }
    const auto& getSurface() const { return myInstance->getSurface(); }

    const auto& getQueueFamilies() const { return myQueueFamilyDescs; }
    uint32_t getGraphicsQueueFamilyIndex() const { return myGraphicsQueueFamilyIndex; }
    uint32_t getTransferQueueFamilyIndex() const { return myTransferQueueFamilyIndex; }
    uint32_t getComputeQueueFamilyIndex() const { return myComputeQueueFamilyIndex; }

    // temp
    QueueHandle<B> getPrimaryGraphicsQueue() const { return getQueueFamilies()[getGraphicsQueueFamilyIndex()].queues[0]; }
    QueueHandle<B> getPrimaryTransferQueue() const { return getQueueFamilies()[getTransferQueueFamilyIndex()].queues[0]; }
    QueueHandle<B> getPrimaryComputeQueue() const { return getQueueFamilies()[getComputeQueueFamilyIndex()].queues[0]; }
    const auto& getGraphicsCommandPools() const { return getQueueFamilies()[getGraphicsQueueFamilyIndex()].commandPools; }
    const auto& getTransferCommandPools() const { return getQueueFamilies()[getTransferQueueFamilyIndex()].commandPools; }
    const auto& getComputeCommandPools() const { return getQueueFamilies()[getComputeQueueFamilyIndex()].commandPools; }
    //

    const auto& getAllocator() const { return myAllocator; }
    const auto& getDescriptorPool() const { return myDescriptorPool; }

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

private:

    std::shared_ptr<InstanceContext<B>> myInstance;
    ScopedFileObject<DeviceConfiguration<B>> myConfig;
    DeviceHandle<B> myDevice = 0;

    std::vector<QueueFamilyDesc<B>> myQueueFamilyDescs;
    uint32_t myGraphicsQueueFamilyIndex = 0; // todo: configure
    uint32_t myTransferQueueFamilyIndex = 1; // todo: configure
    uint32_t myComputeQueueFamilyIndex = 2; // todo: configure
    
    AllocatorHandle<B> myAllocator = 0;
	DescriptorPoolHandle<B> myDescriptorPool = 0;

    SemaphoreHandle<B> myTimelineSemaphore = 0;
    std::atomic_uint64_t myTimelineValue = 0;

    // todo: make to an atomic queue!!
    std::shared_mutex myTimelineCallbacksMutex;
    std::list<std::pair<uint64_t, std::function<void(uint64_t)>>> myTimelineCallbacks;
};
