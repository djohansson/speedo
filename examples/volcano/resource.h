#pragma once

#include "device.h"
#include "gfx-types.h"

#include <atomic>
#include <string>


template <GraphicsBackend B>
struct ResourceCreateDesc
{
    const char* name = nullptr;
};

template <GraphicsBackend B>
class Resource
{
public:

    Resource(Resource<B>&& other);
    Resource(
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        const ResourceCreateDesc<B>& desc,
        ObjectType<B> objectType,
        uint64_t objectHandle);
    virtual ~Resource();

    const auto& getName() const { return myName; }

protected:

    const auto& getDeviceContext() const { return myDeviceContext; }

private:

    std::shared_ptr<DeviceContext<B>> myDeviceContext;
    std::string myName;

#ifdef PROFILING_ENABLED
    static std::atomic_uint32_t sTypeCount;
#endif
};
