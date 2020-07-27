#pragma once

#include "file.h"
#include "gfx-types.h"

#include <any>
#include <vector>


template <GraphicsBackend B>
struct InstanceConfiguration
{
    InstanceConfiguration();
    
    std::string applicationName = "volcano";
    std::string engineName = "magma";
    ApplicationInfo<B> appInfo = {};

    template <class Archive>
    void load(Archive& archive)
    {
        archive(
            CEREAL_NVP(applicationName),
            CEREAL_NVP(engineName),
            CEREAL_NVP(appInfo)
        );

        appInfo.pApplicationName = applicationName.c_str();
        appInfo.pEngineName = engineName.c_str();
    }

    template <class Archive>
    void save(Archive& archive) const
    {
        archive(
            CEREAL_NVP(applicationName),
            CEREAL_NVP(engineName),
            CEREAL_NVP(appInfo)
        );
    }
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
    PhysicalDeviceFeautures<B> deviceFeatures = {};
    std::vector<QueueFamilyProperties<B>> queueFamilyProperties;
    std::vector<uint32_t> queueFamilyPresentSupport;
};

template <GraphicsBackend B>
class InstanceContext
{
public:

    InstanceContext(InstanceContext&& other) noexcept = default;
    InstanceContext(AutoReadWriteJSONFileObject<InstanceConfiguration<B>>&& config, void* surfaceHandle);
    ~InstanceContext();

    const auto& getConfig() const { return myConfig; }
    const auto& getInstance() const { return myInstance; }
    const auto& getSurface() const { return mySurface; }
    const auto& getPhysicalDevices() const { return myPhysicalDevices; }
    const auto& getPhysicalDeviceInfos() const { return myPhysicalDeviceInfos; }
    const auto& getGraphicsDeviceCandidates() const { return myGraphicsDeviceCandidates; }

    void updateSurfaceCapabilities(uint32_t physicalDeviceIndex);

private:

    AutoReadWriteJSONFileObject<InstanceConfiguration<B>> myConfig;
    InstanceHandle<B> myInstance;
    SurfaceHandle<B> mySurface;
    std::vector<PhysicalDeviceHandle<B>> myPhysicalDevices;
    std::vector<PhysicalDeviceInfo<B>> myPhysicalDeviceInfos;
    std::vector<std::pair<uint32_t, uint32_t>> myGraphicsDeviceCandidates;
    std::any myUserData;
};
