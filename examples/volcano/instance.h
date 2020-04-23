#pragma once

#include "gfx-types.h"
#include "utils.h"

#include <any>
#include <vector>

template <GraphicsBackend B>
struct InstanceDesc
{
    InstanceCreateDesc<B> createDesc;
    void* surfaceHandle = nullptr;
};

template <GraphicsBackend B>
class InstanceContext : Noncopyable
{
public:

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
