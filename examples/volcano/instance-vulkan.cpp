#include "instance.h"
#include "vk-utils.h"

#include <algorithm>
#include <vector>

namespace instance
{

static PFN_vkGetPhysicalDeviceProperties2 vkGetPhysicalDeviceProperties2 = {};
static PFN_vkGetPhysicalDeviceFeatures2 vkGetPhysicalDeviceFeatures2 = {};

SurfaceCapabilities<Vk> getSurfaceCapabilities(
    SurfaceHandle<Vk> surface,
    PhysicalDeviceHandle<Vk> device)
{
    SurfaceCapabilities<Vk> capabilities;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface, &capabilities);

    return capabilities;
}

PhysicalDeviceInfo<Vk> getPhysicalDeviceInfo(
	SurfaceHandle<Vk> surface,
    InstanceHandle<Vk> instance,
	PhysicalDeviceHandle<Vk> device)
{
    PhysicalDeviceInfo<Vk> deviceInfo = {};

    instance::vkGetPhysicalDeviceFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
        vkGetInstanceProcAddr(
            instance,
            "vkGetPhysicalDeviceFeatures2"));
    deviceInfo.deviceFeaturesEx.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    deviceInfo.deviceFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    deviceInfo.deviceFeatures.pNext = &deviceInfo.deviceFeaturesEx;
    instance::vkGetPhysicalDeviceFeatures2(device, &deviceInfo.deviceFeatures);

    instance::vkGetPhysicalDeviceProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
        vkGetInstanceProcAddr(
            instance,
            "vkGetPhysicalDeviceProperties2"));
    deviceInfo.devicePropertiesEx.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES;
    deviceInfo.deviceProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    deviceInfo.deviceProperties.pNext = &deviceInfo.devicePropertiesEx;
    instance::vkGetPhysicalDeviceProperties2(device, &deviceInfo.deviceProperties);
    
	deviceInfo.swapchainInfo.capabilities = getSurfaceCapabilities(surface, device);

	uint32_t formatCount;
	vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, nullptr);
	if (formatCount != 0)
	{
		deviceInfo.swapchainInfo.formats.resize(formatCount);
		vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount,
											 deviceInfo.swapchainInfo.formats.data());
	}

	uint32_t presentModeCount;
	vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, nullptr);
	if (presentModeCount != 0)
	{
		deviceInfo.swapchainInfo.presentModes.resize(presentModeCount);
		vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount,
												  deviceInfo.swapchainInfo.presentModes.data());
	}

    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
    if (queueFamilyCount != 0)
	{
        deviceInfo.queueFamilyProperties.resize(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, deviceInfo.queueFamilyProperties.data());

        deviceInfo.queueFamilyPresentSupport.resize(queueFamilyCount);
        for (uint32_t queueFamilyIt = 0; queueFamilyIt < queueFamilyCount; queueFamilyIt++)
            vkGetPhysicalDeviceSurfaceSupportKHR(device, queueFamilyIt, surface, &deviceInfo.queueFamilyPresentSupport[queueFamilyIt]);
    }
    
	return deviceInfo;
}

struct UserData
{
    VkDebugUtilsMessengerEXT debugUtilsMessenger = 0;
};

VkBool32 debugUtilsMessengerCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT messageTypes,
    const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
    void* pUserData)
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
        __debugbreak();

    return VK_FALSE;
}

VkDebugUtilsMessengerCreateInfoEXT debugUtilsMessengerCallbackCreateInfo =
{
    VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
    nullptr,
    0,
    VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT,
    VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
    debugUtilsMessengerCallback,
    nullptr
};

VkDebugUtilsMessengerEXT createDebugUtilsMessenger(VkInstance instance)
{
    auto vkCreateDebugUtilsMessengerEXT =
        (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
    assert(vkCreateDebugUtilsMessengerEXT != nullptr);

    VkDebugUtilsMessengerEXT messenger;
    VK_CHECK(vkCreateDebugUtilsMessengerEXT(instance, &debugUtilsMessengerCallbackCreateInfo, nullptr, &messenger));

    return messenger;
}

}

template <>
InstanceConfiguration<Vk>::InstanceConfiguration()
: appInfo{
    VK_STRUCTURE_TYPE_APPLICATION_INFO,
    nullptr,
    nullptr,
    VK_MAKE_VERSION(1, 0, 0),
    nullptr,
    VK_MAKE_VERSION(1, 0, 0),
    VK_API_VERSION_1_2}
{
}

template <>
void InstanceContext<Vk>::updateSurfaceCapabilities(PhysicalDeviceHandle<Vk> device)
{
    myPhysicalDeviceInfos[device].swapchainInfo.capabilities = instance::getSurfaceCapabilities(mySurface, device);
}

template <>
InstanceContext<Vk>::InstanceContext(
    AutoSaveJSONFileObject<InstanceConfiguration<Vk>>&& config,
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
    VK_CHECK(vkEnumerateInstanceLayerProperties(&instanceLayerCount, nullptr));
    std::cout << instanceLayerCount << " layers found!\n";
    if (instanceLayerCount > 0)
    {
        std::unique_ptr<VkLayerProperties[]> instanceLayers(
            new VkLayerProperties[instanceLayerCount]);
        VK_CHECK(vkEnumerateInstanceLayerProperties(&instanceLayerCount, instanceLayers.get()));
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

    // must be sorted lexicographically for std::includes to work!
    std::sort(instanceExtensions.begin(), instanceExtensions.end(),
        [](const char* lhs, const char* rhs) { return strcmp(lhs, rhs) < 0; });

    std::vector<const char*> requiredLayers = {};
    if constexpr (PROFILING_ENABLED)
        requiredLayers.emplace_back("VK_LAYER_KHRONOS_validation");

    std::vector<const char*> requiredExtensions = {
        // must be sorted lexicographically for std::includes to work!
#if defined(__APPLE__)
        "VK_EXT_metal_surface",
#endif
        "VK_KHR_get_physical_device_properties2",
        "VK_KHR_surface",
#if defined(__WINDOWS__)
        "VK_KHR_win32_surface",
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

    if constexpr(PROFILING_ENABLED)
        requiredExtensions.emplace_back("VK_EXT_debug_utils");

    // must be sorted lexicographically for std::includes to work!
    std::sort(requiredExtensions.begin(), requiredExtensions.end(),
        [](const char* lhs, const char* rhs) { return strcmp(lhs, rhs) < 0; });

    assertf(std::includes(
        instanceExtensions.begin(), instanceExtensions.end(), requiredExtensions.begin(),
        requiredExtensions.end(),
        [](const char* lhs, const char* rhs) { return strcmp(lhs, rhs) < 0; }), "Can't find required Vulkan extensions.");

    VkInstanceCreateInfo info = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    info.pNext = &instance::debugUtilsMessengerCallbackCreateInfo;
    info.pApplicationInfo = &myConfig.appInfo;
    info.enabledLayerCount = static_cast<uint32_t>(requiredLayers.size());
    info.ppEnabledLayerNames = info.enabledLayerCount ? requiredLayers.data() : nullptr;
    info.enabledExtensionCount = static_cast<uint32_t>(requiredExtensions.size());
    info.ppEnabledExtensionNames = info.enabledExtensionCount ? requiredExtensions.data() : nullptr;

    VK_CHECK(vkCreateInstance(&info, nullptr, &myInstance));

    mySurface = createSurface(myInstance, surfaceHandle);

    uint32_t physicalDeviceCount = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(myInstance, &physicalDeviceCount, nullptr));
    if (physicalDeviceCount == 0)
        throw std::runtime_error("failed to find GPUs with Vulkan support!");

    myPhysicalDevices.resize(physicalDeviceCount);
    VK_CHECK(vkEnumeratePhysicalDevices(myInstance, &physicalDeviceCount, myPhysicalDevices.data()));

    myGraphicsDeviceCandidates.reserve(myPhysicalDevices.size());
    
    for (uint32_t deviceIt = 0; deviceIt < myPhysicalDevices.size(); deviceIt++)
    {
        auto physicalDevice = myPhysicalDevices[deviceIt];
        auto infoInsertNode = myPhysicalDeviceInfos.emplace(
            physicalDevice,
            instance::getPhysicalDeviceInfo(
                mySurface,
                myInstance,
                physicalDevice));
        
        auto& physicalDeviceInfo = infoInsertNode.first->second;
        // pointers set up in instance::getPhysicalDeviceInfo will be invalid (pointing to the stack), so patch these up here
        physicalDeviceInfo.deviceProperties.pNext = &physicalDeviceInfo.devicePropertiesEx;
        physicalDeviceInfo.deviceFeatures.pNext = &physicalDeviceInfo.deviceFeaturesEx;
        //

        for (uint32_t queueFamilyIt = 0; queueFamilyIt < physicalDeviceInfo.queueFamilyProperties.size(); queueFamilyIt++)
        {
            const auto& queueFamilyProperties = physicalDeviceInfo.queueFamilyProperties[queueFamilyIt];
            const auto& queueFamilyPresentSupport = physicalDeviceInfo.queueFamilyPresentSupport[queueFamilyIt];

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
        return deviceTypePriority[myPhysicalDeviceInfos[myPhysicalDevices[lhs.first]].deviceProperties.properties.deviceType] < 
            deviceTypePriority[myPhysicalDeviceInfos[myPhysicalDevices[rhs.first]].deviceProperties.properties.deviceType];
    });

    myUserData = instance::UserData();

    if constexpr(PROFILING_ENABLED)
    {
        std::any_cast<instance::UserData>(&myUserData)->debugUtilsMessenger =
            instance::createDebugUtilsMessenger(myInstance);
    }
}

template <>
InstanceContext<Vk>::~InstanceContext()
{
    ZoneScopedN("~Instance()");

    if constexpr(PROFILING_ENABLED)
    {
        auto vkDestroyDebugUtilsMessengerEXT =
            (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(myInstance, "vkDestroyDebugUtilsMessengerEXT");
        vkDestroyDebugUtilsMessengerEXT(myInstance, std::any_cast<instance::UserData>(&myUserData)->debugUtilsMessenger, nullptr);
    }

    vkDestroySurfaceKHR(myInstance, mySurface, nullptr);
    vkDestroyInstance(myInstance, nullptr);
}
