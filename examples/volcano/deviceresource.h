#pragma once

#include "device.h"
#include "gfx-types.h"

#include <atomic>
#include <list>
#include <string>

template <GraphicsBackend B>
struct DeviceResourceCreateDesc
{
    const char* name = nullptr;
};

template <GraphicsBackend B>
class DeviceResource
{
public:

    virtual ~DeviceResource();

    const auto& getName() const { return myName; }

protected:

    DeviceResource(DeviceResource<B>&& other) = default;
    DeviceResource( // no object names are set
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        const DeviceResourceCreateDesc<B>& desc);
    DeviceResource( // uses desc.name and one objectType for all objectHandles
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        const DeviceResourceCreateDesc<B>& desc,
        uint32_t objectCount,
        ObjectType<B> objectType,
        const uint64_t* objectHandles);
    DeviceResource( // uses objectNames and individual objectTypes for all objectHandles
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        const DeviceResourceCreateDesc<B>& desc,
        uint32_t objectCount,
        const ObjectType<B>* objectTypes,
        const uint64_t* objectHandles,
        const char** objectNames);

    const auto& getDeviceContext() const { return myDeviceContext; }
    
    void setObjectName(
        ObjectType<B> objectType,
        uint64_t objectHandle,
        const char* objectName);

private:

    std::shared_ptr<DeviceContext<B>> myDeviceContext;
    std::string myName;
    std::list<std::string> myObjectNames;
};
