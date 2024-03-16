#include "../instance.h"

#include "utils.h"

#include <algorithm>
#include <vector>
#include <iostream>

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

void getPhysicalDeviceInfo(PhysicalDeviceInfo<Vk>& deviceInfo, InstanceHandle<Vk> instance, PhysicalDeviceHandle<Vk> device)
{
	auto vkGetPhysicalDeviceFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
		vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceFeatures2"));

	deviceInfo.deviceRobustnessFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT;
	deviceInfo.inlineUniformBlockFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INLINE_UNIFORM_BLOCK_FEATURES;
	deviceInfo.inlineUniformBlockFeatures.pNext = &deviceInfo.deviceRobustnessFeatures;
	deviceInfo.inlineUniformBlockFeatures.inlineUniformBlock = true;
	deviceInfo.deviceFeaturesEx.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
	deviceInfo.deviceFeaturesEx.pNext = &deviceInfo.inlineUniformBlockFeatures;
	deviceInfo.deviceFeaturesEx.timelineSemaphore = true;
	
	deviceInfo.deviceFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
	deviceInfo.deviceFeatures.pNext = &deviceInfo.deviceFeaturesEx;
	vkGetPhysicalDeviceFeatures2(device, &deviceInfo.deviceFeatures);

	auto vkGetPhysicalDeviceProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
		vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties2"));

	deviceInfo.devicePropertiesEx.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES;
	deviceInfo.devicePropertiesEx.pNext = &deviceInfo.inlineUniformBlockProperties;
	deviceInfo.inlineUniformBlockProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INLINE_UNIFORM_BLOCK_PROPERTIES;
	deviceInfo.inlineUniformBlockProperties.maxDescriptorSetInlineUniformBlocks = 1;
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
	for (uint32_t objectIt = 0; objectIt < pCallbackData->objectCount; objectIt++)
	{
		std::cerr << "Object " << objectIt;

		if (pCallbackData->pObjects[objectIt].pObjectName)
			std::cerr << ", \"" << pCallbackData->pObjects[objectIt].pObjectName << "\"";

		std::cerr << ": ";
	}

	if (pCallbackData->pMessageIdName)
		std::cerr << pCallbackData->pMessageIdName << ": ";

	std::cerr << pCallbackData->pMessage << std::endl;

	if (messageSeverity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
		__debugbreak();

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
void Instance<Vk>::updateSurfaceCapabilities(
	PhysicalDeviceHandle<Vk> device, SurfaceHandle<Vk> surface)
{
	myPhysicalDeviceSwapchainInfos.at(std::make_tuple(device, surface)).capabilities =
		instance::getSurfaceCapabilities(device, surface);
}

template <>
const SwapchainInfo<Vk>&
Instance<Vk>::updateSwapchainInfo(PhysicalDeviceHandle<Vk> device, SurfaceHandle<Vk> surface)
{
	ZoneScopedN("Instance::updateSwapchainInfo");
	
	auto infoInsertNode = myPhysicalDeviceSwapchainInfos.emplace(
		std::make_tuple(device, surface),
		instance::getPhysicalDeviceSwapchainInfo(device, surface));
	
	return infoInsertNode.first->second;
}

template <>
const SwapchainInfo<Vk>&
Instance<Vk>::getSwapchainInfo(PhysicalDeviceHandle<Vk> device, SurfaceHandle<Vk> surface) const
{
	ZoneScopedN("Instance::getSwapchainInfo");

	return myPhysicalDeviceSwapchainInfos.at(std::make_tuple(device, surface));
}

template <>
Instance<Vk>::Instance(InstanceConfiguration<Vk>&& defaultConfig)
: myConfig(std::forward<InstanceConfiguration<Vk>>(defaultConfig))
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

	static const char* VK_DRIVER_FILES_STR = "VK_DRIVER_FILES";
	if (char* vkDriverFiles = getenv(VK_DRIVER_FILES_STR))
		std::cout << VK_DRIVER_FILES_STR << "=" << vkDriverFiles << '\n';
#endif

	uint32_t instanceLayerCount;
	VK_CHECK(vkEnumerateInstanceLayerProperties(&instanceLayerCount, nullptr));
	std::cout << instanceLayerCount << " vulkan layer(s) found:" << '\n';
	if (instanceLayerCount > 0)
	{
		auto instanceLayers = std::make_unique<VkLayerProperties[]>(instanceLayerCount);
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
// #if GRAPHICS_VALIDATION_ENABLED
// 	static constexpr std::string_view validationLayerName = "VK_LAYER_KHRONOS_validation";

// 	requiredLayers.emplace_back(validationLayerName.data());

// 	const VkBool32 settingValidateCore = VK_TRUE;
// 	const VkBool32 settingValidateSync = VK_TRUE;
// 	const VkBool32 settingThreadSafety = VK_TRUE;
// 	const char* settingDebugAction[] = {"VK_DBG_LAYER_ACTION_LOG_MSG"};
// 	const char* settingReportFlags[] = {"info", "warn", "perf", "error", "debug"};
// 	const VkBool32 settingEnableMessageLimit = VK_TRUE;
// 	const int32_t settingDuplicateMessageLimit = 3;

// 	const VkLayerSettingEXT settings[] = {
// 		{validationLayerName.data(), "validate_core", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &settingValidateCore},
// 		{validationLayerName.data(), "validate_sync", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &settingValidateSync},
// 		{validationLayerName.data(), "thread_safety", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &settingThreadSafety},
// 		{validationLayerName.data(), "debug_action", VK_LAYER_SETTING_TYPE_STRING_EXT, 1, settingDebugAction},
// 		{validationLayerName.data(), "report_flags", VK_LAYER_SETTING_TYPE_STRING_EXT, static_cast<uint32_t>(std::size(settingReportFlags)), settingReportFlags},
// 		{validationLayerName.data(), "enable_message_limit", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &settingEnableMessageLimit},
// 		{validationLayerName.data(), "duplicate_message_limit", VK_LAYER_SETTING_TYPE_INT32_EXT, 1, &settingDuplicateMessageLimit}};

// 	const VkLayerSettingsCreateInfoEXT layerSettingsCreateInfo = {
// 		VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT, &instance::debugUtilsMessengerCallbackCreateInfo,
// 		static_cast<uint32_t>(std::size(settings)), settings};
// #endif

	std::vector<const char*> requiredExtensions = {
	// must be sorted lexicographically for std::includes to work!
#if defined(__OSX__)
		"VK_EXT_metal_surface",
		"VK_KHR_portability_enumeration",
		"VK_KHR_surface",
#elif defined(__WINDOWS__)
		"VK_KHR_surface",
		"VK_KHR_win32_surface",
#elif defined(__linux__)
		"VK_KHR_surface",
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

#if GRAPHICS_VALIDATION_ENABLED
	requiredExtensions.emplace_back("VK_EXT_debug_utils");
#endif

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
// #if GRAPHICS_VALIDATION_ENABLED
// 	info.pNext = &layerSettingsCreateInfo;
// #endif
	info.pApplicationInfo = &myConfig.appInfo;
	info.enabledLayerCount = static_cast<uint32_t>(requiredLayers.size());
	info.ppEnabledLayerNames = info.enabledLayerCount ? requiredLayers.data() : nullptr;
	info.enabledExtensionCount = static_cast<uint32_t>(requiredExtensions.size());
	info.ppEnabledExtensionNames = info.enabledExtensionCount ? requiredExtensions.data() : nullptr;
#if defined(__OSX__)
	info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif

	VK_CHECK(vkCreateInstance(&info, &myHostAllocationCallbacks, &myInstance));

	uint32_t physicalDeviceCount = 0;
	VK_CHECK(vkEnumeratePhysicalDevices(myInstance, &physicalDeviceCount, nullptr));
	if (physicalDeviceCount == 0)
		throw std::runtime_error("failed to find GPUs with Vulkan support!");

	myPhysicalDevices.resize(physicalDeviceCount);
	VK_CHECK(
		vkEnumeratePhysicalDevices(myInstance, &physicalDeviceCount, myPhysicalDevices.data()));

	for (uint32_t physicalDeviceIt = 0; physicalDeviceIt < myPhysicalDevices.size(); physicalDeviceIt++)
	{
		auto physicalDevice = myPhysicalDevices[physicalDeviceIt];
		auto infoInsertNode = myPhysicalDeviceInfos.emplace(physicalDevice, std::make_unique<PhysicalDeviceInfo<Vk>>());
		instance::getPhysicalDeviceInfo(*infoInsertNode.first->second, myInstance, physicalDevice);
	}

	myUserData = instance::UserData();

	if constexpr (GRAPHICS_VALIDATION_ENABLED)
	{
		auto vkCreateDebugUtilsMessengerEXT = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(
			myInstance, "vkCreateDebugUtilsMessengerEXT"));
		assert(vkCreateDebugUtilsMessengerEXT != nullptr);

		VkDebugUtilsMessengerEXT messenger;
		VK_CHECK(vkCreateDebugUtilsMessengerEXT(
			myInstance, &instance::debugUtilsMessengerCallbackCreateInfo, &myHostAllocationCallbacks, &messenger));

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

	vkDestroyInstance(myInstance, &myHostAllocationCallbacks);
}
