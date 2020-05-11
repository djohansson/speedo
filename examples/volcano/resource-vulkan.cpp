#include "resource.h"
#include "vk-utils.h"


namespace resource
{

void setObjectName(
    DeviceHandle<GraphicsBackend::Vulkan> device,
    ObjectType<GraphicsBackend::Vulkan> objectType,
    uint64_t objectHandle,
    const char* objectName)
{
    // look into caching this function pointer if needed
    auto vkSetDebugUtilsObjectNameEXT = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
        vkGetDeviceProcAddr(device, "vkSetDebugUtilsObjectNameEXT"));

    auto imageNameInfo = VkDebugUtilsObjectNameInfoEXT{
        VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
        nullptr,
        objectType,
        objectHandle,
        objectName,
    };
    CHECK_VK(vkSetDebugUtilsObjectNameEXT(device, &imageNameInfo));
}

}

#ifdef PROFILING_ENABLED
template <> 
std::atomic_uint32_t Resource<GraphicsBackend::Vulkan>::sTypeCount(0);
#endif

template <> 
Resource<GraphicsBackend::Vulkan>::Resource(Resource<GraphicsBackend::Vulkan>&& other)
: myDeviceContext(other.myDeviceContext)
, myName(std::move(other.myName))
{
#ifdef PROFILING_ENABLED
    sTypeCount++;
#endif
}
    
template <> 
Resource<GraphicsBackend::Vulkan>::Resource(
    const std::shared_ptr<DeviceContext<GraphicsBackend::Vulkan>>& deviceContext,
    const ResourceCreateDesc<GraphicsBackend::Vulkan>& desc,
    ObjectType<GraphicsBackend::Vulkan> objectType,
    uint64_t objectHandle)
: myDeviceContext(deviceContext)
, myName(desc.name)
{
#ifdef PROFILING_ENABLED
    sTypeCount++;
#endif
    resource::setObjectName(deviceContext->getDevice(), objectType, objectHandle, myName.c_str());
}

template <> 
Resource<GraphicsBackend::Vulkan>::~Resource()
{
#ifdef PROFILING_ENABLED
    sTypeCount--;
#endif
}
