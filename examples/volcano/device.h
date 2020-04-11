#pragma once

#include "command.h"
#include "frame.h"
#include "gfx-types.h"
#include "swapchain.h"
#include "utils.h"

#include <any>
#include <atomic>
#include <cassert>
#include <optional>
#include <vector>


template <GraphicsBackend B>
struct DeviceCreateDesc
{
    InstanceHandle<B> instance = 0;
    SurfaceHandle<B> surface = 0;
    uint16_t commandBufferThreadCount = 2;
};

template <GraphicsBackend B>
class DeviceContext : Noncopyable
{
public:

	DeviceContext(DeviceCreateDesc<B>&& desc);
    ~DeviceContext();

    const auto& getDesc() const { return myDesc; }
    const auto getDevice() const { return myDevice; }
    const auto getPhysicalDevice() const { return myPhysicalDevice; }
    const auto& getPhysicalDeviceProperties() const { return myPhysicalDeviceProperties; }
    const auto getSelectedQueue() const { return mySelectedQueue; }
    const auto getSelectedQueueFamilyIndex() const { return mySelectedQueueFamilyIndex; }
    const auto& getSwapchainConfiguration() const { return mySwapchainConfiguration; }
    const auto getAllocator() const { return myAllocator; }
    const auto getDescriptorPool() const { return myDescriptorPool; }
    const auto getFrameCommandPool(uint16_t poolIndex) const { return myFrameCommandPools[poolIndex]; }
    const auto getTransferCommandPool() const { return myTransferCommandPool; }

    void createFrameCommands(Frame<B>& frame) const;
    CommandContext<B> createTransferCommands() const;

private:

    const DeviceCreateDesc<B> myDesc = {};
    DeviceHandle<B> myDevice = 0;
    std::vector<PhysicalDeviceHandle<GraphicsBackend::Vulkan>> myPhysicalDevices;
    PhysicalDeviceHandle<B> myPhysicalDevice = 0;
    PhysicalDeviceProperties<B> myPhysicalDeviceProperties = {};
    QueueHandle<B> mySelectedQueue = 0;
    std::optional<uint32_t> mySelectedQueueFamilyIndex;
    SwapchainConfiguration<B> mySwapchainConfiguration = {};
    AllocatorHandle<B> myAllocator = 0;
	DescriptorPoolHandle<B> myDescriptorPool = 0;
    std::vector<CommandPoolHandle<B>> myFrameCommandPools;
	CommandPoolHandle<B> myTransferCommandPool = 0;
    SemaphoreHandle<B> myTransferTimelineSemaphore = 0;
    std::shared_ptr<std::atomic_uint64_t> myTransferTimelineValue = 0;
    std::any myUserData;
};
