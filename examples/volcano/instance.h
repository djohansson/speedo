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

private:

    const InstanceDesc<B> myInstanceDesc;
    InstanceHandle<B> myInstance;
    SurfaceHandle<B> mySurface;
    std::any myUserData;
};
