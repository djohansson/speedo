#pragma once

#include "gfx-types.h"
#include "swapchain.h"

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

    auto getDevice() const { return myDevice; }
    auto getPhysicalDevice() const { return myPhysicalDevice; }
    auto getPhysicalDeviceProperties() const { return myPhysicalDeviceProperties; }

    auto getSwapchainInfo() const { return mySwapChainInfo; }

    auto getSelectedQueue() const { return mySelectedQueue; }
    auto getSelectedQueueFamilyIndex() const { return mySelectedQueueFamilyIndex; }
    auto getSelectedSurfaceFormat() const { return mySelectedSurfaceFormat; }
    auto getSelectedPresentMode() const { return mySelectedPresentMode; }
    auto getSelectedFrameCount() const { return mySelectedFrameCount; }

private:

    const DeviceCreateDesc<B> myDesc = {};

    DeviceHandle<B> myDevice = 0;
    PhysicalDeviceHandle<B> myPhysicalDevice = 0;
    PhysicalDeviceProperties<B> myPhysicalDeviceProperties = {};
    
    SwapchainInfo<B> mySwapChainInfo = {};
    
    QueueHandle<B> mySelectedQueue = 0;
    int mySelectedQueueFamilyIndex = -1;
    SurfaceFormat<GraphicsBackend::Vulkan> mySelectedSurfaceFormat = {};
	PresentMode<GraphicsBackend::Vulkan> mySelectedPresentMode = {};
    uint32_t mySelectedFrameCount = 0;
};
