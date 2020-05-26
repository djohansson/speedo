#pragma once

#include "device.h"
#include "gfx-types.h"

#include <list>
#include <string>
#include <map>
#include <shared_mutex>

template <GraphicsBackend B>
struct DeviceResourceCreateDesc
{
    std::string name;
};

template <GraphicsBackend B>
class DeviceResource
{
public:

    virtual ~DeviceResource();

    const auto& getName() const { return myName; }
    static uint32_t getTypeCount(ObjectType<B> type);

protected:

    struct Object
    {
        std::string name;
        ObjectType<B> type = {};
        uint64_t handle = 0;
    };

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

    const auto& getDeviceContext() const { return myDeviceContext; }

    void addObject(
        ObjectType<B> objectType,
        uint64_t objectHandle,
        const char* objectName);

private:

    void internalAddObject(
        ObjectType<B> objectType,
        uint64_t objectHandle,
        const char* objectName);

    static void incrementTypeCount(ObjectType<B> type, uint32_t count);
    static void decrementTypeCount(ObjectType<B> type, uint32_t count);

    std::shared_ptr<DeviceContext<B>> myDeviceContext;
    std::string myName;
    std::list<Object> myObjects;

    static std::shared_mutex gObjectTypeCountsMutex;
    static std::map<ObjectType<B>, uint32_t> gObjectTypeCounts;
};
