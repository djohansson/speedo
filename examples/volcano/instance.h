#pragma once

#include "file.h"
#include "gfx-types.h"

#include <any>
#include <vector>


template <GraphicsBackend B>
struct InstanceCreateDesc
{
    InstanceCreateDesc() = default;
    InstanceCreateDesc(InstanceCreateDesc&& other) = default;

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
    void load(Archive& archive);

    template <class Archive>
    void save(Archive& archive) const;
};

template <GraphicsBackend B>
struct InstanceDesc
{
    ScopedJSONFileObject<InstanceCreateDesc<B>> createDesc = {};
    void* surfaceHandle = nullptr;
};

template <GraphicsBackend B>
class InstanceContext
{
public:

    InstanceContext(InstanceContext&& other) = default;
    InstanceContext(InstanceDesc<B>&& desc);
    ~InstanceContext();

    const auto& getInstanceDesc() const { return myInstanceDesc; }
    const auto& getInstance() const { return myInstance; }
    const auto& getSurface() const { return mySurface; }
    const auto& getPhysicalDevices() const { return myPhysicalDevices; }

private:

    const InstanceDesc<B> myInstanceDesc;
    InstanceHandle<B> myInstance;
    SurfaceHandle<B> mySurface;
    std::vector<PhysicalDeviceHandle<B>> myPhysicalDevices;
    std::any myUserData;
};

#include "instance-vulkan.h"
