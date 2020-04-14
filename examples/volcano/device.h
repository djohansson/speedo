#pragma once

#include "gfx-types.h"
#include "instance.h"
#include "utils.h"

#include <any>
#include <atomic>
#include <memory>
#include <optional>
#include <vector>


template <GraphicsBackend B>
struct DeviceDesc
{
    std::shared_ptr<InstanceContext<B>> instanceContext;
    uint16_t commandBufferThreadCount = 2;
};

template <GraphicsBackend B>
struct SwapchainConfiguration
{
	SurfaceCapabilities<B> capabilities = {};
	std::vector<SurfaceFormat<B>> formats;
	std::vector<PresentMode<B>> presentModes;
	uint8_t selectedFormatIndex = 0;
	uint8_t selectedPresentModeIndex = 0;
    uint8_t selectedImageCount = 2;
	
	const auto selectedFormat() const { return formats[selectedFormatIndex]; };
	const auto selectedPresentMode() const { return presentModes[selectedPresentModeIndex]; };
};

template <GraphicsBackend B>
class DeviceContext : public std::enable_shared_from_this<DeviceContext<B>>, private Noncopyable
{
public:

	DeviceContext(DeviceDesc<B>&& desc);
    ~DeviceContext();

    const auto& getDeviceDesc() const { return myDeviceDesc; }
    const auto& getDevice() const { return myDevice; }
    const auto& getPhysicalDevice() const { return myPhysicalDevice; }
    const auto& getPhysicalDeviceProperties() const { return myPhysicalDeviceProperties; }
    const auto& getSelectedQueue() const { return mySelectedQueue; }
    const auto& getSelectedQueueFamilyIndex() const { return mySelectedQueueFamilyIndex; }
    const auto& getSwapchainConfiguration() const { return mySwapchainConfiguration; }
    const auto& getAllocator() const { return myAllocator; }
    const auto& getDescriptorPool() const { return myDescriptorPool; }
    const auto& getFrameCommandPools() const { return myFrameCommandPools; }
    const auto& getTransferCommandPool() const { return myTransferCommandPool; }

private:

    const DeviceDesc<B> myDeviceDesc = {};
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
    std::any myUserData;
};
