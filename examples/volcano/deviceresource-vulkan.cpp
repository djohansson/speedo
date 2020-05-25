#include "deviceresource.h"
#include "vk-utils.h"

#include <core/slang-secure-crt.h>


template <> 
void DeviceResource<GraphicsBackend::Vulkan>::setObjectName(
    const std::shared_ptr<DeviceContext<GraphicsBackend::Vulkan>>& deviceContext,
    ObjectType<GraphicsBackend::Vulkan> objectType,
    uint64_t objectHandle,
    const std::string& objectName)
{
    auto device = deviceContext->getDevice();

    // todo: create a lookup table for device extensions functions
    auto vkSetDebugUtilsObjectNameEXT = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
        vkGetDeviceProcAddr(device, "vkSetDebugUtilsObjectNameEXT"));

    auto imageNameInfo = VkDebugUtilsObjectNameInfoEXT{
        VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
        nullptr,
        objectType,
        objectHandle,
        objectName.c_str()};
    CHECK_VK(vkSetDebugUtilsObjectNameEXT(device, &imageNameInfo));
}

template <> 
DeviceResource<GraphicsBackend::Vulkan>::DeviceResource(
    const std::shared_ptr<DeviceContext<GraphicsBackend::Vulkan>>& deviceContext,
    const DeviceResourceCreateDesc<GraphicsBackend::Vulkan>& desc)
: myDeviceContext(deviceContext)
, myName(desc.name)
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

        setObjectName(
            myDeviceContext,
            objectType,
            objectHandles[objectIt],
            myObjectNames.emplace_back(stringBuffer));
    } 
}
    
template <> 
DeviceResource<GraphicsBackend::Vulkan>::DeviceResource(
    const std::shared_ptr<DeviceContext<GraphicsBackend::Vulkan>>& deviceContext,
    const DeviceResourceCreateDesc<GraphicsBackend::Vulkan>& desc,
    uint32_t objectCount,
    const ObjectType<GraphicsBackend::Vulkan>* objectTypes,
    const uint64_t* objectHandles,
    const char** objectNames)
: DeviceResource(deviceContext, desc)
{
    for (uint32_t objectIt = 0; objectIt < objectCount; objectIt++)
        setObjectName(
            myDeviceContext,
            objectTypes[objectIt],
            objectHandles[objectIt],
            myObjectNames.emplace_back(objectNames[objectIt]));
}

template <> 
DeviceResource<GraphicsBackend::Vulkan>::~DeviceResource()
{
}
