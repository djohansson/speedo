#pragma once

#include "file.h"
#include "types.h"
#include "utils.h"

#include <any>
#include <vector>
#include <string>

template <GraphicsBackend B>
struct InstanceConfiguration
{
    InstanceConfiguration();
    
    std::string applicationName = "volcano";
    std::string engineName = "magma";
    ApplicationInfo<B> appInfo = {};
};

template <GraphicsBackend B>
struct SwapchainInfo
{
	SurfaceCapabilities<B> capabilities = {};
	std::vector<SurfaceFormat<B>> formats;
	std::vector<PresentMode<B>> presentModes;
};

template <GraphicsBackend B>
struct PhysicalDeviceInfo
{
    SwapchainInfo<B> swapchainInfo = {};
    PhysicalDeviceProperties<B> deviceProperties = {};
    PhysicalDevicePropertiesEx<B> devicePropertiesEx = {};
    PhysicalDeviceFeatures<B> deviceFeatures = {};
    PhysicalDeviceFeaturesEx<B> deviceFeaturesEx = {};
    PhysicalDeviceRobustnessFeatures<B> deviceRobustnessFeatures = {};
    std::vector<QueueFamilyProperties<B>> queueFamilyProperties;
    std::vector<uint32_t> queueFamilyPresentSupport;
};

template <GraphicsBackend B>
class InstanceContext : public Noncopyable
{
public:

    InstanceContext(AutoSaveJSONFileObject<InstanceConfiguration<B>>&& config, void* windowHandle);
    ~InstanceContext();

    const auto& getConfig() const { return myConfig; }
    auto getInstance() const { return myInstance; }
    auto getSurface() const { return mySurface; }
    const auto& getPhysicalDevices() const { return myPhysicalDevices; }
    const auto& getPhysicalDeviceInfo(PhysicalDeviceHandle<B> device) { return myPhysicalDeviceInfos[device]; }
    const auto& getGraphicsDeviceCandidates() const { return myGraphicsDeviceCandidates; }

    void updateSurfaceCapabilities(PhysicalDeviceHandle<B> device);

private:

    AutoSaveJSONFileObject<InstanceConfiguration<B>> myConfig;
    InstanceHandle<B> myInstance;
    SurfaceHandle<B> mySurface;
    std::vector<PhysicalDeviceHandle<B>> myPhysicalDevices;
    UnorderedMap<PhysicalDeviceHandle<B>, PhysicalDeviceInfo<B>> myPhysicalDeviceInfos;
    std::vector<std::pair<uint32_t, uint32_t>> myGraphicsDeviceCandidates;
    std::any myUserData;
};

#include "instance.inl"
#include "instance-vulkan.inl"