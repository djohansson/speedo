#include "instance.h"
#include "profiling.h"
#include "vk-utils.h"
#include "volcano.h"

#include <algorithm>
#include <vector>

#include <mimalloc.h>

namespace instance
{

SurfaceCapabilities<Vk>
getSurfaceCapabilities(PhysicalDeviceHandle<Vk> device, SurfaceHandle<Vk> surface)
{
	SurfaceCapabilities<Vk> capabilities;
	vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface, &capabilities);

	return capabilities;
}

PhysicalDeviceInfo<Vk>
getPhysicalDeviceInfo(InstanceHandle<Vk> instance, PhysicalDeviceHandle<Vk> device)
{
	auto vkGetPhysicalDeviceFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
		vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceFeatures2"));

	PhysicalDeviceInfo<Vk> deviceInfo{};
	deviceInfo.deviceRobustnessFeatures.sType =
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT;
	deviceInfo.deviceFeaturesEx.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
	deviceInfo.deviceFeaturesEx.pNext = &deviceInfo.deviceRobustnessFeatures;
	deviceInfo.deviceFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
	deviceInfo.deviceFeatures.pNext = &deviceInfo.deviceFeaturesEx;
	vkGetPhysicalDeviceFeatures2(device, &deviceInfo.deviceFeatures);

	auto vkGetPhysicalDeviceProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
		vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties2"));

	deviceInfo.devicePropertiesEx.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES;
	deviceInfo.deviceProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
	deviceInfo.deviceProperties.pNext = &deviceInfo.devicePropertiesEx;
	vkGetPhysicalDeviceProperties2(device, &deviceInfo.deviceProperties);

	uint32_t queueFamilyCount = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
	if (queueFamilyCount != 0)
	{
		deviceInfo.queueFamilyProperties.resize(queueFamilyCount);
		vkGetPhysicalDeviceQueueFamilyProperties(
			device, &queueFamilyCount, deviceInfo.queueFamilyProperties.data());
	}

	return deviceInfo;
}

SwapchainInfo<Vk>
getPhysicalDeviceSwapchainInfo(PhysicalDeviceHandle<Vk> device, SurfaceHandle<Vk> surface)
{
	SwapchainInfo<Vk> swapchainInfo{};
	swapchainInfo.capabilities = getSurfaceCapabilities(device, surface);

	uint32_t formatCount;
	vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, nullptr);
	if (formatCount != 0)
	{
		swapchainInfo.formats.resize(formatCount);
		vkGetPhysicalDeviceSurfaceFormatsKHR(
			device, surface, &formatCount, swapchainInfo.formats.data());
	}

	uint32_t presentModeCount;
	vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, nullptr);
	if (presentModeCount != 0)
	{
		swapchainInfo.presentModes.resize(presentModeCount);
		vkGetPhysicalDeviceSurfacePresentModesKHR(
			device, surface, &presentModeCount, swapchainInfo.presentModes.data());
	}

	uint32_t queueFamilyCount = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
	if (queueFamilyCount != 0)
	{
		swapchainInfo.queueFamilyPresentSupport.resize(queueFamilyCount);
		for (uint32_t queueFamilyIt = 0; queueFamilyIt < queueFamilyCount; queueFamilyIt++)
			vkGetPhysicalDeviceSurfaceSupportKHR(
				device,
				queueFamilyIt,
				surface,
				&swapchainInfo.queueFamilyPresentSupport[queueFamilyIt]);
	}

	return swapchainInfo;
}

struct UserData
{
	VkDebugUtilsMessengerEXT debugUtilsMessenger{};
	VkAllocationCallbacks allocationCallbacks{};
};

VkBool32 debugUtilsMessengerCallback(
	VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
	VkDebugUtilsMessageTypeFlagsEXT messageTypes,
	const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
	void* pUserData)
{
	if (pCallbackData->messageIdNumber !=
		-1666394502) // UNASSIGNED-CoreValidation-DrawState-QueryNotReset - https://github.com/KhronosGroup/Vulkan-ValidationLayers/issues/1179
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
	}

	return VK_FALSE;
}

VkDebugUtilsMessengerCreateInfoEXT debugUtilsMessengerCallbackCreateInfo{
	VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
	nullptr,
	0,
	VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
		VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT, // | VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT,
	VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
		VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
	debugUtilsMessengerCallback,
	nullptr};

} // namespace instance

template <>
InstanceConfiguration<Vk>::InstanceConfiguration()
	: applicationName("volcano")
	, engineName("magma")
	, appInfo{
		  VK_STRUCTURE_TYPE_APPLICATION_INFO,
		  nullptr,
		  nullptr,
		  VK_MAKE_VERSION(1, 0, 0),
		  nullptr,
		  VK_MAKE_VERSION(1, 0, 0),
		  VK_API_VERSION_1_3}
{}

template <>
void Instance<Vk>::updateSurfaceCapabilities(
	PhysicalDeviceHandle<Vk> device, SurfaceHandle<Vk> surface)
{
	myPhysicalDeviceSwapchainInfos.at(std::make_tuple(device, surface)).capabilities =
		instance::getSurfaceCapabilities(device, surface);
}

template <>
const SwapchainInfo<Vk>&
Instance<Vk>::getSwapchainInfo(PhysicalDeviceHandle<Vk> device, SurfaceHandle<Vk> surface)
{
	auto infoInsertNode = myPhysicalDeviceSwapchainInfos.emplace(
		std::make_tuple(device, surface),
		instance::getPhysicalDeviceSwapchainInfo(device, surface));

	auto& swapchainInfo = infoInsertNode.first->second;

	return swapchainInfo;
}

template <>
Instance<Vk>::Instance(InstanceConfiguration<Vk>&& defaultConfig)
	: myConfig(AutoSaveJSONFileObject<InstanceConfiguration<Vk>>(
		  std::filesystem::path(volcano_getUserProfilePath()) / "instance.json",
		  std::forward<InstanceConfiguration<Vk>>(defaultConfig)))
	, myHostAllocationCallbacks{
		nullptr,
		[](void* /*pUserData*/, size_t size, size_t alignment, VkSystemAllocationScope /*allocationScope*/)
		{
			return mi_malloc_aligned(size, alignment);
		},
		[](void* /*pUserData*/, void* pOriginal, size_t size, size_t alignment, VkSystemAllocationScope /*allocationScope*/)
		{
			return mi_realloc_aligned(pOriginal, size, alignment);
		},
		[](void* /*pUserData*/, void* pMemory)
		{
			mi_free(pMemory);
		},
		nullptr,
		nullptr}
{
	ZoneScopedN("Instance()");

#ifdef _DEBUG
	static const char* VK_LOADER_DEBUG_STR = "VK_LOADER_DEBUG";
	if (char* vkLoaderDebug = getenv(VK_LOADER_DEBUG_STR))
		std::cout << VK_LOADER_DEBUG_STR << "=" << vkLoaderDebug << '\n';

	static const char* VK_LAYER_PATH_STR = "VK_LAYER_PATH";
	if (char* vkLayerPath = getenv(VK_LAYER_PATH_STR))
		std::cout << VK_LAYER_PATH_STR << "=" << vkLayerPath << '\n';

	static const char* VK_ICD_FILENAMES_STR = "VK_ICD_FILENAMES";
	if (char* vkIcdFilenames = getenv(VK_ICD_FILENAMES_STR))
		std::cout << VK_ICD_FILENAMES_STR << "=" << vkIcdFilenames << '\n';
#endif

	uint32_t instanceLayerCount;
	VK_CHECK(vkEnumerateInstanceLayerProperties(&instanceLayerCount, nullptr));
	std::cout << instanceLayerCount << " vulkan layer(s) found:" << '\n';
	if (instanceLayerCount > 0)
	{
		std::unique_ptr<VkLayerProperties[]> instanceLayers(
			new VkLayerProperties[instanceLayerCount]);
		VK_CHECK(vkEnumerateInstanceLayerProperties(&instanceLayerCount, instanceLayers.get()));
		for (uint32_t i = 0; i < instanceLayerCount; ++i)
			std::cout << instanceLayers[i].layerName << '\n';
	}

	uint32_t instanceExtensionCount;
	vkEnumerateInstanceExtensionProperties(nullptr, &instanceExtensionCount, nullptr);

	std::vector<VkExtensionProperties> availableInstanceExtensions(instanceExtensionCount);
	vkEnumerateInstanceExtensionProperties(
		nullptr, &instanceExtensionCount, availableInstanceExtensions.data());

	std::cout << instanceExtensionCount << " vulkan instance extension(s) found:" << '\n';

	std::vector<const char*> instanceExtensions(instanceExtensionCount);
	for (uint32_t i = 0; i < instanceExtensionCount; i++)
	{
		instanceExtensions[i] = availableInstanceExtensions[i].extensionName;
		std::cout << instanceExtensions[i] << '\n';
	}

	// must be sorted lexicographically for std::includes to work!
	std::sort(
		instanceExtensions.begin(),
		instanceExtensions.end(),
		[](const char* lhs, const char* rhs) { return strcmp(lhs, rhs) < 0; });

	std::vector<const char*> requiredLayers = {};
	if constexpr (GRAPHICS_VALIDATION_ENABLED)
		requiredLayers.emplace_back("VK_LAYER_KHRONOS_validation");

	std::vector<const char*> requiredExtensions = {
	// must be sorted lexicographically for std::includes to work!
#if defined(__APPLE__)
		"VK_EXT_metal_surface",
#endif
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

	if constexpr (GRAPHICS_VALIDATION_ENABLED)
		requiredExtensions.emplace_back("VK_EXT_debug_utils");

	// must be sorted lexicographically for std::includes to work!
	std::sort(
		requiredExtensions.begin(),
		requiredExtensions.end(),
		[](const char* lhs, const char* rhs) { return strcmp(lhs, rhs) < 0; });

	assertf(
		std::includes(
			instanceExtensions.begin(),
			instanceExtensions.end(),
			requiredExtensions.begin(),
			requiredExtensions.end(),
			[](const char* lhs, const char* rhs) { return strcmp(lhs, rhs) < 0; }),
		"Can't find required Vulkan extensions.");

	VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	info.pNext = &instance::debugUtilsMessengerCallbackCreateInfo;
	info.pApplicationInfo = &myConfig.appInfo;
	info.enabledLayerCount = static_cast<uint32_t>(requiredLayers.size());
	info.ppEnabledLayerNames = info.enabledLayerCount ? requiredLayers.data() : nullptr;
	info.enabledExtensionCount = static_cast<uint32_t>(requiredExtensions.size());
	info.ppEnabledExtensionNames = info.enabledExtensionCount ? requiredExtensions.data() : nullptr;

	VK_CHECK(vkCreateInstance(&info, &myHostAllocationCallbacks, &myInstance));

	uint32_t physicalDeviceCount = 0;
	VK_CHECK(vkEnumeratePhysicalDevices(myInstance, &physicalDeviceCount, nullptr));
	if (physicalDeviceCount == 0)
		throw std::runtime_error("failed to find GPUs with Vulkan support!");

	myPhysicalDevices.resize(physicalDeviceCount);
	VK_CHECK(
		vkEnumeratePhysicalDevices(myInstance, &physicalDeviceCount, myPhysicalDevices.data()));

	for (uint32_t physicalDeviceIt = 0; physicalDeviceIt < myPhysicalDevices.size();
		 physicalDeviceIt++)
	{
		auto physicalDevice = myPhysicalDevices[physicalDeviceIt];
		auto infoInsertNode = myPhysicalDeviceInfos.emplace(
			physicalDevice, instance::getPhysicalDeviceInfo(myInstance, physicalDevice));

		auto& physicalDeviceInfo = infoInsertNode.first->second;
		// pointers set up in instance::getPhysicalDeviceInfo will be invalid (pointing to the stack), so patch these up here
		physicalDeviceInfo.deviceProperties.pNext = &physicalDeviceInfo.devicePropertiesEx;
		physicalDeviceInfo.deviceFeaturesEx.pNext = &physicalDeviceInfo.deviceRobustnessFeatures;
		physicalDeviceInfo.deviceFeatures.pNext = &physicalDeviceInfo.deviceFeaturesEx;
		//
	}

	myUserData = instance::UserData();

	if constexpr (GRAPHICS_VALIDATION_ENABLED)
	{
		auto vkCreateDebugUtilsMessengerEXT = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(
			myInstance, "vkCreateDebugUtilsMessengerEXT"));
		assert(vkCreateDebugUtilsMessengerEXT != nullptr);

		VkDebugUtilsMessengerEXT messenger;
		VK_CHECK(vkCreateDebugUtilsMessengerEXT(
			myInstance, &instance::debugUtilsMessengerCallbackCreateInfo, &myHostAllocator, &messenger));

		std::any_cast<instance::UserData>(&myUserData)->debugUtilsMessenger = messenger;
	}
}

template <>
Instance<Vk>::~Instance()
{
	ZoneScopedN("~Instance()");

	if constexpr (GRAPHICS_VALIDATION_ENABLED)
	{
		auto vkDestroyDebugUtilsMessengerEXT =
			(PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
				myInstance, "vkDestroyDebugUtilsMessengerEXT");
		vkDestroyDebugUtilsMessengerEXT(
			myInstance,
			std::any_cast<instance::UserData>(&myUserData)->debugUtilsMessenger,
			&getHostAllocationCallbacks());
	}

	vkDestroyInstance(myInstance, &myHostAllocator);
}
