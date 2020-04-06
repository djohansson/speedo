#pragma once

#include "gfx-types.h"
#include "swapchain.h"
#include "utils.h"

#include <optional>
#include <vector>

template <GraphicsBackend B>
struct DeviceCreateDesc
{
    InstanceHandle<B> instance = 0;
    SurfaceHandle<B> surface = 0;
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

private:

    const DeviceCreateDesc<B> myDesc = {};
    DeviceHandle<B> myDevice = 0;

    std::vector<PhysicalDeviceHandle<GraphicsBackend::Vulkan>> myPhysicalDevices;
    PhysicalDeviceHandle<B> myPhysicalDevice = 0;
    PhysicalDeviceProperties<B> myPhysicalDeviceProperties = {};
    
    QueueHandle<B> mySelectedQueue = 0;
    std::optional<uint32_t> mySelectedQueueFamilyIndex;

    SwapchainConfiguration<B> mySwapchainConfiguration = {};

    // tmp. todo: make generic
    //TracyVkCtx myTracyContext;
    // todo: set up on transfer commandlist
    //myTracyContext = TracyVkContext(myDeviceContext->getPhysicalDevice(), myDeviceContext->getDevice(), myDeviceContext->getSelectedQueue(), window.frames[0].commandBuffers[0]);
    //TracyVkDestroy(myTracyContext);
    //
};
