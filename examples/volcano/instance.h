#pragma once

#include "file.h"
#include "gfx-types.h"

#include <any>
#include <vector>


template <GraphicsBackend B>
struct InstanceConfiguration
{
    InstanceConfiguration() = default;
    InstanceConfiguration(InstanceConfiguration&& other)
    : applicationName(std::move(other.applicationName))
    , engineName(std::move(other.engineName))
    , appInfo(std::move(other.appInfo))
    {}

    std::string applicationName = "volcano";
    std::string engineName = "magma";
    ApplicationInfo<B> appInfo = {
        VK_STRUCTURE_TYPE_APPLICATION_INFO,
        nullptr,
        nullptr,
        VK_MAKE_VERSION(1, 0, 0),
        nullptr,
        VK_MAKE_VERSION(1, 0, 0),
        VK_API_VERSION_1_2
    };

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

    InstanceContext(InstanceContext&& other) = default;
    InstanceContext(ScopedFileObject<InstanceConfiguration<B>>&& config, void* surfaceHandle);
    ~InstanceContext();

    const auto& getConfig() const { return myConfig; }
    const auto& getInstance() const { return myInstance; }
    const auto& getSurface() const { return mySurface; }
    const auto& getPhysicalDevices() const { return myPhysicalDevices; }
    const auto& getPhysicalDeviceInfos() const { return myPhysicalDeviceInfos; }
    const auto& getGraphicsDeviceCandidates() const { return myGraphicsDeviceCandidates; }

    void updateSurfaceCapabilities(uint32_t physicalDeviceIndex);

private:

    ScopedFileObject<InstanceConfiguration<B>> myConfig;
    InstanceHandle<B> myInstance;
    SurfaceHandle<B> mySurface;
    std::vector<PhysicalDeviceHandle<B>> myPhysicalDevices;
    std::vector<PhysicalDeviceInfo<B>> myPhysicalDeviceInfos;
    std::vector<std::pair<uint32_t, uint32_t>> myGraphicsDeviceCandidates;
    std::any myUserData;
};
