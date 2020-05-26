#include "deviceresource.h"
#include "vk-utils.h"

#include <core/slang-secure-crt.h>


template <>
std::shared_mutex DeviceResource<GraphicsBackend::Vulkan>::gObjectTypeCountsMutex{};

template <>
std::map<ObjectType<GraphicsBackend::Vulkan>, uint32_t> DeviceResource<GraphicsBackend::Vulkan>::gObjectTypeCounts{};

template <>
void DeviceResource<GraphicsBackend::Vulkan>::internalAddObject(
    ObjectType<GraphicsBackend::Vulkan> objectType,
    uint64_t objectHandle,
    const char* objectName)
{
    auto device = getDeviceContext()->getDevice();

    // todo: create a lookup table for device extensions functions
    auto vkSetDebugUtilsObjectNameEXT = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
        vkGetDeviceProcAddr(device, "vkSetDebugUtilsObjectNameEXT"));

    auto imageNameInfo = VkDebugUtilsObjectNameInfoEXT{
        VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
        nullptr,
        objectType,
        objectHandle,
        objectName};

    CHECK_VK(vkSetDebugUtilsObjectNameEXT(device, &imageNameInfo));

    myObjects.emplace_back(Object{objectName, objectType, objectHandle});
}

template <>
void DeviceResource<GraphicsBackend::Vulkan>::incrementTypeCount(ObjectType<GraphicsBackend::Vulkan> type, uint32_t count)
{
    gObjectTypeCounts[type] += count;
}

template <>
void DeviceResource<GraphicsBackend::Vulkan>::decrementTypeCount(ObjectType<GraphicsBackend::Vulkan> type, uint32_t count)
{
    gObjectTypeCounts[type] -= count;
}

template <>
void DeviceResource<GraphicsBackend::Vulkan>::addObject(
    ObjectType<GraphicsBackend::Vulkan> objectType,
    uint64_t objectHandle,
    const char* objectName)
{
    internalAddObject(objectType, objectHandle, objectName);
    std::unique_lock writelock(gObjectTypeCountsMutex);
    incrementTypeCount(objectType, 1);
}

template <> 
DeviceResource<GraphicsBackend::Vulkan>::DeviceResource(
    const std::shared_ptr<DeviceContext<GraphicsBackend::Vulkan>>& deviceContext,
    const DeviceResourceCreateDesc<GraphicsBackend::Vulkan>& desc)
: myDeviceContext(deviceContext)
, myName(std::move(desc.name))
{
}

template <> 
DeviceResource<GraphicsBackend::Vulkan>::DeviceResource(
    const std::shared_ptr<DeviceContext<GraphicsBackend::Vulkan>>& deviceContext,
    const DeviceResourceCreateDesc<GraphicsBackend::Vulkan>& desc,
    uint32_t objectCount,
    ObjectType<GraphicsBackend::Vulkan> objectType,
    const uint64_t* objectHandles)
: DeviceResource(deviceContext, desc)
{
    char stringBuffer[256];
    for (uint32_t objectIt = 0; objectIt < objectCount; objectIt++)
    {
        sprintf_s(
            stringBuffer,
            sizeof(stringBuffer),
            "%.*s%.*u",
            getName().size(),
            getName().c_str(),
            2,
            objectIt);

        internalAddObject(
            objectType,
            objectHandles[objectIt],
            stringBuffer);
    }

    std::unique_lock writelock(gObjectTypeCountsMutex);
    incrementTypeCount(objectType, objectCount);
}

template <>
DeviceResource<GraphicsBackend::Vulkan>::~DeviceResource()
{
    std::unique_lock writeLock(gObjectTypeCountsMutex);
    for (const auto& object : myObjects)
        decrementTypeCount(object.type, 1);
}

template <>
uint32_t DeviceResource<GraphicsBackend::Vulkan>::getTypeCount(ObjectType<GraphicsBackend::Vulkan> type)
{
    std::shared_lock readLock(gObjectTypeCountsMutex);
    return gObjectTypeCounts[type];
}
