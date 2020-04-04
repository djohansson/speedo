#include "instance.h"
#include "gfx.h"

#include <algorithm>
#include <vector>

namespace instance_vulkan
{

struct InstanceData
{
    VkDebugReportCallbackEXT debugCallback = VK_NULL_HANDLE;
};

VkDebugReportCallbackEXT createDebugCallback(VkInstance instance)
{
    VkDebugReportCallbackCreateInfoEXT debugCallbackInfo = {};
    debugCallbackInfo.sType = VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT;
    debugCallbackInfo.flags =
        /* VK_DEBUG_REPORT_INFORMATION_BIT_EXT |*/ VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT |
        VK_DEBUG_REPORT_DEBUG_BIT_EXT | VK_DEBUG_REPORT_ERROR_BIT_EXT |
        VK_DEBUG_REPORT_WARNING_BIT_EXT;

    debugCallbackInfo.pfnCallback = static_cast<PFN_vkDebugReportCallbackEXT>(
        [](VkDebugReportFlagsEXT flags, VkDebugReportObjectTypeEXT /*objectType*/, uint64_t /*object*/,
        size_t /*location*/, int32_t /*messageCode*/, const char* layerPrefix, const char* message,
        void* /*userData*/) -> VkBool32 {
            std::cout << layerPrefix << ": " << message << std::endl;

            // VK_DEBUG_REPORT_INFORMATION_BIT_EXT = 0x00000001,
            // VK_DEBUG_REPORT_WARNING_BIT_EXT = 0x00000002,
            // VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT = 0x00000004,
            // VK_DEBUG_REPORT_ERROR_BIT_EXT = 0x00000008,
            // VK_DEBUG_REPORT_DEBUG_BIT_EXT = 0x00000010,

            if (flags & VK_DEBUG_REPORT_ERROR_BIT_EXT || flags & VK_DEBUG_REPORT_WARNING_BIT_EXT)
                return VK_TRUE;

            return VK_FALSE;
        });

    auto vkCreateDebugReportCallbackEXT = (PFN_vkCreateDebugReportCallbackEXT)vkGetInstanceProcAddr(
        instance, "vkCreateDebugReportCallbackEXT");
    assert(vkCreateDebugReportCallbackEXT != nullptr);

    VkDebugReportCallbackEXT debugCallback = VK_NULL_HANDLE;
    CHECK_VK(vkCreateDebugReportCallbackEXT(instance, &debugCallbackInfo, nullptr, &debugCallback));

    return debugCallback;
}

}

template <>
InstanceContext<GraphicsBackend::Vulkan>::InstanceContext(InstanceCreateDesc<GraphicsBackend::Vulkan>&& desc)
: myDesc(std::move(desc))
{
    uint32_t instanceLayerCount;
    CHECK_VK(vkEnumerateInstanceLayerProperties(&instanceLayerCount, nullptr));
    std::cout << instanceLayerCount << " layers found!\n";
    if (instanceLayerCount > 0)
    {
        std::unique_ptr<VkLayerProperties[]> instanceLayers(
            new VkLayerProperties[instanceLayerCount]);
        CHECK_VK(vkEnumerateInstanceLayerProperties(&instanceLayerCount, instanceLayers.get()));
        for (uint32_t i = 0; i < instanceLayerCount; ++i)
            std::cout << instanceLayers[i].layerName << "\n";
    }

    const char* enabledLayerNames[] =
    {
    #ifdef PROFILING_ENABLED
        "VK_LAYER_LUNARG_standard_validation",
    #endif
    };

    uint32_t instanceExtensionCount;
    vkEnumerateInstanceExtensionProperties(nullptr, &instanceExtensionCount, nullptr);

    std::vector<VkExtensionProperties> availableInstanceExtensions(instanceExtensionCount);
    vkEnumerateInstanceExtensionProperties(
        nullptr, &instanceExtensionCount, availableInstanceExtensions.data());

    std::vector<const char*> instanceExtensions(instanceExtensionCount);
    for (uint32_t i = 0; i < instanceExtensionCount; i++)
    {
        instanceExtensions[i] = availableInstanceExtensions[i].extensionName;
        std::cout << instanceExtensions[i] << "\n";
    }

    std::sort(
        instanceExtensions.begin(), instanceExtensions.end(),
        [](const char* lhs, const char* rhs) { return strcmp(lhs, rhs) < 0; });

    std::vector<const char*> requiredExtensions = {
        // must be sorted lexicographically for std::includes to work!
        "VK_EXT_debug_report",
        "VK_KHR_surface",
#if defined(__WINDOWS__)
        "VK_KHR_win32_surface",
#elif defined(__APPLE__)
        "VK_MVK_macos_surface",
#elif defined(__linux__)
#	if defined(VK_USE_PLATFORM_XCB_KHR)
        "VK_KHR_xcb_surface",
#	elif defined(VK_USE_PLATFORM_XLIB_KHR)
        "VK_KHR_xlib_surface",
#	elif defined(VK_USE_PLATFORM_WAYLAND_KHR)
        "VK_KHR_wayland_surface",
#	else // default to xcb
        "VK_KHR_xcb_surface",
#	endif
#endif
    };

    assert(std::includes(
        instanceExtensions.begin(), instanceExtensions.end(), requiredExtensions.begin(),
        requiredExtensions.end(),
        [](const char* lhs, const char* rhs) { return strcmp(lhs, rhs) < 0; }));

    // if (std::find(instanceExtensions.begin(), instanceExtensions.end(), "VK_KHR_display") ==
    // instanceExtensions.end()) 	instanceExtensions.push_back("VK_KHR_display");

    VkInstanceCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &myDesc;
    info.enabledLayerCount = sizeof_array(enabledLayerNames);
    info.ppEnabledLayerNames = info.enabledLayerCount ? enabledLayerNames : nullptr;
    info.enabledExtensionCount = static_cast<uint32_t>(requiredExtensions.size());
    info.ppEnabledExtensionNames = requiredExtensions.data();

    CHECK_VK(vkCreateInstance(&info, nullptr, &myInstance));

    myData = instance_vulkan::InstanceData();

    std::any_cast<instance_vulkan::InstanceData>(&myData)->debugCallback = instance_vulkan::createDebugCallback(myInstance);
}

template <>
InstanceContext<GraphicsBackend::Vulkan>::~InstanceContext()
{
    auto vkDestroyDebugReportCallbackEXT =
        (PFN_vkDestroyDebugReportCallbackEXT)vkGetInstanceProcAddr(
            myInstance, "vkDestroyDebugReportCallbackEXT");
    assert(vkDestroyDebugReportCallbackEXT != nullptr);
    vkDestroyDebugReportCallbackEXT(myInstance, std::any_cast<instance_vulkan::InstanceData>(&myData)->debugCallback, nullptr);

    vkDestroyInstance(myInstance, nullptr);
}
