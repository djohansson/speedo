#include "instance.h"
#include "gfx.h"
#include "vk-utils.h"

#include <algorithm>
#include <vector>


namespace instance_vulkan
{

struct UserData
{
#ifdef PROFILING_ENABLED
    VkDebugUtilsMessengerEXT debugUtilsMessenger = 0;
#endif
};

#ifdef PROFILING_ENABLED
VkDebugUtilsMessengerEXT createDebugUtilsMessenger(VkInstance instance)
{
    auto vkCreateDebugUtilsMessengerEXT =
        (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
    assert(vkCreateDebugUtilsMessengerEXT != nullptr);

    auto callback = static_cast<PFN_vkDebugUtilsMessengerCallbackEXT>([](
        VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
        VkDebugUtilsMessageTypeFlagsEXT messageTypes,
        const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
        void* pUserData) -> VkBool32
    {
        for (uint32_t objectIt = 0; objectIt < pCallbackData->objectCount; objectIt++)
        {
            std::cout << "Object " << objectIt;

            if (pCallbackData->pObjects[objectIt].pObjectName)
                std::cout << ", \"" << pCallbackData->pObjects[objectIt].pObjectName << "\"";
            
            std::cout << ": ";
        }

        if (pCallbackData->pMessageIdName)
            std::cout << pCallbackData->pMessageIdName << ": ";

        std::cout << pCallbackData->pMessage << std::endl;

        if (messageSeverity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
            return VK_TRUE;
    
        return VK_FALSE;
    });

    VkDebugUtilsMessengerCreateInfoEXT callbackCreateInfo =
    {
        VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        nullptr,
        0,
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT,
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        callback,
        nullptr
    };

    VkDebugUtilsMessengerEXT messenger;
    CHECK_VK(vkCreateDebugUtilsMessengerEXT(instance, &callbackCreateInfo, nullptr, &messenger));

    return messenger;
}
#endif

}

template <>
void InstanceContext<GraphicsBackend::Vulkan>::updateSurfaceCapabilities(uint32_t physicalDeviceIndex)
{
    myPhysicalDeviceInfos[physicalDeviceIndex].swapchainInfo.capabilities = 
        getSurfaceCapabilities<GraphicsBackend::Vulkan>(mySurface, myPhysicalDevices[physicalDeviceIndex]);
}

template <>
InstanceContext<GraphicsBackend::Vulkan>::InstanceContext(
    ScopedFileObject<InstanceConfiguration<GraphicsBackend::Vulkan>>&& config,
    void* surfaceHandle)
: myConfig(std::move(config))
{
    ZoneScopedN("Instance()");

#ifdef _DEBUG
	static const char* VK_LOADER_DEBUG_STR = "VK_LOADER_DEBUG";
	if (char* vkLoaderDebug = getenv(VK_LOADER_DEBUG_STR))
		std::cout << VK_LOADER_DEBUG_STR << "=" << vkLoaderDebug << std::endl;

	static const char* VK_LAYER_PATH_STR = "VK_LAYER_PATH";
	if (char* vkLayerPath = getenv(VK_LAYER_PATH_STR))
		std::cout << VK_LAYER_PATH_STR << "=" << vkLayerPath << std::endl;

	static const char* VK_ICD_FILENAMES_STR = "VK_ICD_FILENAMES";
	if (char* vkIcdFilenames = getenv(VK_ICD_FILENAMES_STR))
		std::cout << VK_ICD_FILENAMES_STR << "=" << vkIcdFilenames << std::endl;
#endif

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

    std::vector<const char*> requiredLayers = {
#ifdef PROFILING_ENABLED
        "VK_LAYER_KHRONOS_validation",
#endif
    };

    std::vector<const char*> requiredExtensions = {
        // must be sorted lexicographically for std::includes to work!
#ifdef PROFILING_ENABLED
        "VK_EXT_debug_utils",
#endif
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

    VkInstanceCreateInfo info = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    info.pApplicationInfo = &myConfig.appInfo;
    info.enabledLayerCount = static_cast<uint32_t>(requiredLayers.size());
    info.ppEnabledLayerNames = info.enabledLayerCount ? requiredLayers.data() : nullptr;
    info.enabledExtensionCount = static_cast<uint32_t>(requiredExtensions.size());
    info.ppEnabledExtensionNames = info.enabledExtensionCount ? requiredExtensions.data() : nullptr;

    CHECK_VK(vkCreateInstance(&info, nullptr, &myInstance));

    mySurface = createSurface(myInstance, surfaceHandle);

    uint32_t physicalDeviceCount = 0;
    CHECK_VK(vkEnumeratePhysicalDevices(myInstance, &physicalDeviceCount, nullptr));
    if (physicalDeviceCount == 0)
        throw std::runtime_error("failed to find GPUs with Vulkan support!");

    myPhysicalDevices.resize(physicalDeviceCount);
    CHECK_VK(vkEnumeratePhysicalDevices(myInstance, &physicalDeviceCount, myPhysicalDevices.data()));

    myPhysicalDeviceInfos.reserve(myPhysicalDevices.size());
    myGraphicsDeviceCandidates.reserve(myPhysicalDevices.size());
    
    for (uint32_t deviceIt = 0; deviceIt < myPhysicalDevices.size(); deviceIt++)
    {
        myPhysicalDeviceInfos.emplace_back(getPhysicalDeviceInfo<GraphicsBackend::Vulkan>(mySurface, myPhysicalDevices[deviceIt]));

        for (uint32_t queueFamilyIt = 0; queueFamilyIt < myPhysicalDeviceInfos.back().queueFamilyProperties.size(); queueFamilyIt++)
        {
            const auto& queueFamilyProperties = myPhysicalDeviceInfos.back().queueFamilyProperties[queueFamilyIt];
            const auto& queueFamilyPresentSupport = myPhysicalDeviceInfos.back().queueFamilyPresentSupport[queueFamilyIt];

            if (queueFamilyProperties.queueFlags & VK_QUEUE_GRAPHICS_BIT && queueFamilyPresentSupport)
                myGraphicsDeviceCandidates.emplace_back(std::make_pair(deviceIt, queueFamilyIt));
        }
    }

    std::sort(myGraphicsDeviceCandidates.begin(), myGraphicsDeviceCandidates.end(), [this](const auto& lhs, const auto& rhs){
        constexpr uint32_t deviceTypePriority[] =
        {
            4,//VK_PHYSICAL_DEVICE_TYPE_OTHER = 0,
            1,//VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU = 1,
            0,//VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU = 2,
            2,//VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU = 3,
            3,//VK_PHYSICAL_DEVICE_TYPE_CPU = 4,
            0x7FFFFFFF//VK_PHYSICAL_DEVICE_TYPE_MAX_ENUM = 0x7FFFFFFF
        };
        return deviceTypePriority[myPhysicalDeviceInfos[lhs.first].deviceProperties.deviceType] < 
            deviceTypePriority[myPhysicalDeviceInfos[rhs.first].deviceProperties.deviceType];
    });

    myUserData = instance_vulkan::UserData();

#ifdef PROFILING_ENABLED
    std::any_cast<instance_vulkan::UserData>(&myUserData)->debugUtilsMessenger =
        instance_vulkan::createDebugUtilsMessenger(myInstance);
#endif
}

template <>
InstanceContext<GraphicsBackend::Vulkan>::~InstanceContext()
{
    ZoneScopedN("~Instance()");

#ifdef PROFILING_ENABLED
    auto vkDestroyDebugUtilsMessengerEXT =
        (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(myInstance, "vkDestroyDebugUtilsMessengerEXT");
    vkDestroyDebugUtilsMessengerEXT(myInstance, std::any_cast<instance_vulkan::UserData>(&myUserData)->debugUtilsMessenger, nullptr);
#endif

    vkDestroySurfaceKHR(myInstance, mySurface, nullptr);
    vkDestroyInstance(myInstance, nullptr);
}
