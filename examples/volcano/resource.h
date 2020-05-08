#pragma once

#include "gfx-types.h"

#include <atomic>
#include <string>

template <typename T>
struct ResourceCreateDesc
{
#ifdef PROFILING_ENABLED
    std::string name;
#endif
};

template <typename T>
class Resource
{
public:

    Resource(Resource&& other) = default;
    Resource(ResourceCreateDesc<T>&& desc);
    ~Resource();

private:

    const ResourceCreateDesc<T> myDesc = {};

#ifdef PROFILING_ENABLED
    static std::atomic_uint32_t typeCount;
#endif
};



// // Must call extension functions through a function pointer:
//    ​PFN_vkSetDebugUtilsObjectNameEXT pfnSetDebugUtilsObjectNameEXT = (PFN_vkSetDebugUtilsObjectNameEXT)vkGetDeviceProcAddr(device, "vkSetDebugUtilsObjectNameEXT");

//    ​// Set a name on the image
//    ​const VkDebugUtilsObjectNameInfoEXT imageNameInfo =
//    ​{
//        ​VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT, // sType
//        ​NULL,                                               // pNext
//        ​VK_OBJECT_TYPE_IMAGE,                               // objectType
//        ​(uint64_t)image,                                    // object
//        ​"Brick Diffuse Texture",                            // pObjectName
//    ​};

//    ​pfnSetDebugUtilsObjectNameEXT(device, &imageNameInfo);