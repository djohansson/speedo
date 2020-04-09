#pragma once

#include "gfx-types.h"
#include "swapchain.h"
#include "utils.h"

#include <any>
#include <cassert>
#include <optional>
#include <tuple>
#include <vector>

template <GraphicsBackend B>
struct CommandCreateDesc
{
    DeviceHandle<B> device = 0;
    QueueHandle<B> queue = 0;
    CommandPoolHandle<B> commandPool = 0;
    // todo: move in the timeline semaphore and its values here
};

template <GraphicsBackend B>
class CommandContext : Noncopyable
{
public:

    CommandContext(CommandContext<B>&& context) = default;
    CommandContext(CommandCreateDesc<B>&& desc);
    ~CommandContext();
    
    const auto getCommandBuffer() const { return myCommandBuffer; }

    void begin() const;
    void submit();
    void end() const;
    bool isComplete() const;
    void sync() const;

private:

    const CommandCreateDesc<B> myDesc = {};
    CommandBufferHandle<B> myCommandBuffer = 0;
    SemaphoreHandle<B> myTimelineSemaphore = 0;
    uint64_t myTimelineValue = 0;
};

template <GraphicsBackend B>
struct DeviceCreateDesc
{
    InstanceHandle<B> instance = 0;
    SurfaceHandle<B> surface = 0;
    uint16_t commandBufferThreadCount = 5;
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
    const auto getFrameCommandPool(uint16_t poolIndex) { return myFrameCommandPools[poolIndex]; }
    const auto getTransferCommandPool() { return myTransferCommandPool; }

    CommandContext<B> createFrameCommands(uint16_t poolIndex) const;
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
	std::vector<CommandPoolHandle<B>> myFrameCommandPools; // count = [myCommandBufferThreadCount]
	CommandPoolHandle<B> myTransferCommandPool = 0;
    std::any myUserData;
};
