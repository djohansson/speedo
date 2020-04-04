#pragma once

#include "gfx-types.h"
#include "swapchain.h"

#include <vector>

template <GraphicsBackend B>
struct DeviceCreateDesc
{
    InstanceHandle<B> instance = 0;
    SurfaceHandle<B> surface = 0;
};

template <GraphicsBackend B>
class DeviceContext
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

    const auto& getSwapchainInfo() const { return mySwapChainInfo; }
    const auto getSelectedSurfaceFormat() const { return mySelectedSurfaceFormat; }
    const auto getSelectedPresentMode() const { return mySelectedPresentMode; }
    const auto getSelectedFrameCount() const { return mySelectedFrameCount; }

private:

    const DeviceCreateDesc<B> myDesc = {};

    std::vector<PhysicalDeviceHandle<GraphicsBackend::Vulkan>> myPhysicalDevices;
    PhysicalDeviceHandle<B> myPhysicalDevice = 0;
    PhysicalDeviceProperties<B> myPhysicalDeviceProperties = {};

    DeviceHandle<B> myDevice = 0;
    
    QueueHandle<B> mySelectedQueue = 0;
    int mySelectedQueueFamilyIndex = -1;

    SwapchainInfo<B> mySwapChainInfo = {};
    SurfaceFormat<GraphicsBackend::Vulkan> mySelectedSurfaceFormat = {};
	PresentMode<GraphicsBackend::Vulkan> mySelectedPresentMode = {};
    uint32_t mySelectedFrameCount = 0;

    // tmp. todo: make generic
    //TracyVkCtx myTracyContext;
    // todo: set up on transfer commandlist
    //myTracyContext = TracyVkContext(myDeviceContext->getPhysicalDevice(), myDeviceContext->getDevice(), myDeviceContext->getSelectedQueue(), window.frames[0].commandBuffers[0]);
    //TracyVkDestroy(myTracyContext);
    //
};
