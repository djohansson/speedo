#pragma once

#include "gfx-types.h"
#include "instance.h"

#include <atomic>
#include <memory>
#include <optional>
#include <vector>


template <GraphicsBackend B>
struct DeviceCreateDesc
{
    DeviceCreateDesc() = default;
    DeviceCreateDesc(
        uint32_t physicalDeviceIndex_)
    : physicalDeviceIndex(physicalDeviceIndex_)
    { }
    DeviceCreateDesc(DeviceCreateDesc&& other)
    : physicalDeviceIndex(std::move(other.physicalDeviceIndex))
    , useCommandPoolReset(std::move(other.useCommandPoolReset))
    , useTimelineSemaphores(std::move(other.useTimelineSemaphores))
    { }

    std::optional<uint32_t> physicalDeviceIndex;
    std::optional<bool> useCommandPoolReset;
    std::optional<bool> useTimelineSemaphores;

    template <class Archive>
    void serialize(Archive& archive);
};

template <GraphicsBackend B>
struct DeviceDesc : ScopedJSONFileObject<DeviceCreateDesc<B>>
{
    std::shared_ptr<InstanceContext<B>> instanceContext;
};

template <GraphicsBackend B>
struct SwapchainConfiguration
{
    SurfaceFormat<B> selectedFormat = {};
	PresentMode<B> selectedPresentMode;
	uint8_t selectedImageCount = 0;	
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
	DeviceContext(DeviceDesc<B>&& desc);
    ~DeviceContext();

    const auto& getDeviceDesc() const { return myDeviceDesc; }
    const auto& getDevice() const { return myDevice; }
    const auto& getPhysicalDevice() const
    { return myDeviceDesc.instanceContext->getPhysicalDevices()[myDeviceDesc.physicalDeviceIndex.value_or(0)]; }

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

    const auto& getSwapchainConfiguration() const { return mySwapchainConfiguration; }

    const auto& getAllocator() const { return myAllocator; }
    const auto& getDescriptorPool() const { return myDescriptorPool; }

private:

    const DeviceDesc<B> myDeviceDesc = {};
    SwapchainConfiguration<B> mySwapchainConfiguration = {};

    DeviceHandle<B> myDevice = 0;

    std::vector<QueueFamilyDesc<B>> myQueueFamilyDescs;
    uint32_t myGraphicsQueueFamilyIndex = 0; // todo: configure
    uint32_t myTransferQueueFamilyIndex = 1; // todo: configure
    uint32_t myComputeQueueFamilyIndex = 2; // todo: configure
    
    AllocatorHandle<B> myAllocator = 0;
	DescriptorPoolHandle<B> myDescriptorPool = 0;
};

#include "device-vulkan.h"