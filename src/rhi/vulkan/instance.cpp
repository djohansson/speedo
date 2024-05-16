#include "../instance.h"

#include "utils.h"

#include <core/profiling.h>

#include <algorithm>
#include <vector>
#include <iostream>

#include <mimalloc.h>

namespace instance
{

static VkDebugUtilsMessengerEXT gdebugUtilsMessenger{};

SurfaceCapabilities<kVk>
GetSurfaceCapabilities(PhysicalDeviceHandle<kVk> device, SurfaceHandle<kVk> surface)
{
	SurfaceCapabilities<kVk> capabilities;
	vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface, &capabilities);

	return capabilities;
}

void GetPhysicalDeviceInfo2(
	PhysicalDeviceInfo<kVk>& deviceInfo,
	InstanceHandle<kVk> instance,
	PhysicalDeviceHandle<kVk> device)
{
	auto vkGetPhysicalDeviceFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
		vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceFeatures2"));

	deviceInfo.inlineUniformBlockFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INLINE_UNIFORM_BLOCK_FEATURES;
	deviceInfo.inlineUniformBlockFeatures.pNext = nullptr;
	deviceInfo.inlineUniformBlockFeatures.inlineUniformBlock = 1u;
	deviceInfo.deviceFeaturesEx.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
	deviceInfo.deviceFeaturesEx.pNext = &deviceInfo.inlineUniformBlockFeatures;
	deviceInfo.deviceFeaturesEx.timelineSemaphore = 1u;

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

SwapchainInfo<kVk>
GetPhysicalDeviceSwapchainInfo(PhysicalDeviceHandle<kVk> device, SurfaceHandle<kVk> surface)
{
	SwapchainInfo<kVk> swapchainInfo{};
	swapchainInfo.capabilities = GetSurfaceCapabilities(device, surface);

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

VkBool32 DebugUtilsMessengerCallback(
	VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
	VkDebugUtilsMessageTypeFlagsEXT messageTypes,
	const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
	void* pUserData)
{
	for (uint32_t objectIt = 0; objectIt < pCallbackData->objectCount; objectIt++)
	{
		std::cerr << "Object " << objectIt;

		if (pCallbackData->pObjects[objectIt].pObjectName != nullptr)
			std::cerr << ", \"" << pCallbackData->pObjects[objectIt].pObjectName << "\"";

		std::cerr << ": ";
	}

	if (pCallbackData->pMessageIdName != nullptr)
		std::cerr << pCallbackData->pMessageIdName << ": ";

	std::cerr << pCallbackData->pMessage << '\n';

	if (messageSeverity > VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
		TRAP();

	return VK_FALSE;
}

VkDebugUtilsMessengerCreateInfoEXT gDebugUtilsMessengerCallbackCreateInfo{
	VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
	nullptr,
	0,
	VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
		VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
		VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
		VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT,
	VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
		VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
	DebugUtilsMessengerCallback,
	nullptr};

} // namespace instance

template <>
void Instance<kVk>::UpdateSurfaceCapabilities(
	PhysicalDeviceHandle<kVk> device, SurfaceHandle<kVk> surface)
{
	myPhysicalDeviceSwapchainInfos.at(std::make_tuple(device, surface)).capabilities =
		instance::GetSurfaceCapabilities(device, surface);
}

template <>
const SwapchainInfo<kVk>&
Instance<kVk>::UpdateSwapchainInfo(PhysicalDeviceHandle<kVk> device, SurfaceHandle<kVk> surface)
{
	ZoneScopedN("Instance::UpdateSwapchainInfo");

	auto infoInsertNode = myPhysicalDeviceSwapchainInfos.emplace(
		std::make_tuple(device, surface),
		instance::GetPhysicalDeviceSwapchainInfo(device, surface));

	return infoInsertNode.first->second;
}

template <>
const SwapchainInfo<kVk>&
Instance<kVk>::GetSwapchainInfo(PhysicalDeviceHandle<kVk> device, SurfaceHandle<kVk> surface) const
{
	ZoneScopedN("Instance::GetSwapchainInfo");

	return myPhysicalDeviceSwapchainInfos.at(std::make_tuple(device, surface));
}

template <>
Instance<kVk>::Instance(InstanceConfiguration<kVk>&& defaultConfig)
: myConfig(std::forward<InstanceConfiguration<kVk>>(defaultConfig))
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
	using namespace instance;

	ZoneScopedN("Instance()");

	if constexpr (GRAPHICS_VALIDATION_LEVEL > 0)
	{
		static const char* gvkLoaderDebugStr = "VK_LOADER_DEBUG";
		if (char* vkLoaderDebug = getenv(gvkLoaderDebugStr))
			std::cout << gvkLoaderDebugStr << "=" << vkLoaderDebug << '\n';

		static const char* gvkLayerPathStr = "VK_LAYER_PATH";
		if (char* vkLayerPath = getenv(gvkLayerPathStr))
			std::cout << gvkLayerPathStr << "=" << vkLayerPath << '\n';

		static const char* gvkDriverFilesStr = "VK_DRIVER_FILES";
		if (char* vkDriverFiles = getenv(gvkDriverFilesStr))
			std::cout << gvkDriverFilesStr << "=" << vkDriverFiles << '\n';
	}

	uint32_t instanceLayerCount;
	VK_CHECK(vkEnumerateInstanceLayerProperties(&instanceLayerCount, nullptr));
	
	if constexpr (GRAPHICS_VALIDATION_LEVEL > 0)
		std::cout << instanceLayerCount << " vulkan layer(s) found:" << '\n';
	
	if (instanceLayerCount > 0)
	{
		auto instanceLayers = std::make_unique<VkLayerProperties[]>(instanceLayerCount);
		VK_CHECK(vkEnumerateInstanceLayerProperties(&instanceLayerCount, instanceLayers.get()));
		if constexpr (GRAPHICS_VALIDATION_LEVEL > 0)
		{
			for (uint32_t i = 0; i < instanceLayerCount; ++i)
				std::cout << instanceLayers[i].layerName << '\n';
		}
	}

	uint32_t instanceExtensionCount;
	vkEnumerateInstanceExtensionProperties(nullptr, &instanceExtensionCount, nullptr);

	std::vector<VkExtensionProperties> availableInstanceExtensions(instanceExtensionCount);
	vkEnumerateInstanceExtensionProperties(
		nullptr, &instanceExtensionCount, availableInstanceExtensions.data());

	if constexpr (GRAPHICS_VALIDATION_LEVEL > 0)
		std::cout << instanceExtensionCount << " vulkan instance extension(s) found:" << '\n';

	std::vector<const char*> instanceExtensions(instanceExtensionCount);
	for (uint32_t i = 0; i < instanceExtensionCount; i++)
	{
		instanceExtensions[i] = availableInstanceExtensions[i].extensionName;
		
		if constexpr (GRAPHICS_VALIDATION_LEVEL > 0)
			std::cout << instanceExtensions[i] << '\n';
	}

	// must be sorted lexicographically for std::includes to work!
	std::sort(
		instanceExtensions.begin(),
		instanceExtensions.end(),
		[](const char* lhs, const char* rhs) { return strcmp(lhs, rhs) < 0; });

	std::vector<const char*> requiredExtensions = {
	// must be sorted lexicographically for std::includes to work!
#if defined(__OSX__)
		"VK_EXT_metal_surface",
		"VK_KHR_portability_enumeration",
		"VK_KHR_surface",
#elif defined(__WINDOWS__)
		"VK_KHR_surface",
		"VK_KHR_win32_surface",
#elif defined(__LINUX__)
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
	std::vector<const char*> requiredLayers = {};

	VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};

	if constexpr (GRAPHICS_VALIDATION_LEVEL > 0)
	{
		requiredExtensions.emplace_back("VK_EXT_debug_utils");

		static constexpr std::string_view kValidationLayerName = "VK_LAYER_KHRONOS_validation";

		requiredLayers.emplace_back(kValidationLayerName.data());

		const VkBool32 settingValidateCore = VK_TRUE;
		const VkBool32 settingValidateSync = VK_FALSE;
		const VkBool32 settingThreadSafety = VK_TRUE;
		const char* settingDebugAction[] = {"VK_DBG_LAYER_ACTION_LOG_MSG"};
		const char* settingReportFlags[] = {"info", "warn", "perf", "error", "debug"};
		const VkBool32 settingEnableMessageLimit = VK_TRUE;
		const int32_t settingDuplicateMessageLimit = 3;

		const VkLayerSettingEXT settings[] = {
			{kValidationLayerName.data(),
			 "validate_core",
			 VK_LAYER_SETTING_TYPE_BOOL32_EXT,
			 1,
			 &settingValidateCore},
			{kValidationLayerName.data(),
			 "validate_sync",
			 VK_LAYER_SETTING_TYPE_BOOL32_EXT,
			 1,
			 &settingValidateSync},
			{kValidationLayerName.data(),
			 "thread_safety",
			 VK_LAYER_SETTING_TYPE_BOOL32_EXT,
			 1,
			 &settingThreadSafety},
			{kValidationLayerName.data(),
			 "debug_action",
			 VK_LAYER_SETTING_TYPE_STRING_EXT,
			 1,
			 settingDebugAction},
			{kValidationLayerName.data(),
			 "report_flags",
			 VK_LAYER_SETTING_TYPE_STRING_EXT,
			 static_cast<uint32_t>(std::size(settingReportFlags)),
			 settingReportFlags},
			{kValidationLayerName.data(),
			 "enable_message_limit",
			 VK_LAYER_SETTING_TYPE_BOOL32_EXT,
			 1,
			 &settingEnableMessageLimit},
			{kValidationLayerName.data(),
			 "duplicate_message_limit",
			 VK_LAYER_SETTING_TYPE_INT32_EXT,
			 1,
			 &settingDuplicateMessageLimit}};

		const VkLayerSettingsCreateInfoEXT layerSettingsCreateInfo = {
			VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT,
			&gDebugUtilsMessengerCallbackCreateInfo,
			static_cast<uint32_t>(std::size(settings)),
			settings};

		info.pNext = &layerSettingsCreateInfo;
	}

	// must be sorted lexicographically for std::includes to work!
	std::sort(
		requiredExtensions.begin(),
		requiredExtensions.end(),
		[](const char* lhs, const char* rhs) { return strcmp(lhs, rhs) < 0; });

	ASSERTF(
		std::includes(
			instanceExtensions.begin(),
			instanceExtensions.end(),
			requiredExtensions.begin(),
			requiredExtensions.end(),
			[](const char* lhs, const char* rhs) { return strcmp(lhs, rhs) < 0; }),
		"Can't find required Vulkan extensions.");

	info.pApplicationInfo = &myConfig.appInfo;
	info.enabledLayerCount = static_cast<uint32_t>(requiredLayers.size());
	info.ppEnabledLayerNames = (info.enabledLayerCount != 0u) ? requiredLayers.data() : nullptr;
	info.enabledExtensionCount = static_cast<uint32_t>(requiredExtensions.size());
	info.ppEnabledExtensionNames =
		(info.enabledExtensionCount != 0u) ? requiredExtensions.data() : nullptr;
#if defined(__OSX__)
	info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif

	VK_CHECK(vkCreateInstance(&info, &myHostAllocationCallbacks, &myInstance));

	uint32_t physicalDeviceCount = 0;
	VK_CHECK(vkEnumeratePhysicalDevices(myInstance, &physicalDeviceCount, nullptr));
	ASSERTF(physicalDeviceCount > 0, "Failed to find GPUs with Vulkan support.");
	
	myPhysicalDevices.resize(physicalDeviceCount);
	VK_CHECK(vkEnumeratePhysicalDevices(myInstance, &physicalDeviceCount, myPhysicalDevices.data()));

	for (auto physicalDevice : myPhysicalDevices)
	{
		auto infoInsertNode = myPhysicalDeviceInfos.emplace(
			physicalDevice, std::make_unique<PhysicalDeviceInfo<kVk>>());
		GetPhysicalDeviceInfo2(*infoInsertNode.first->second, myInstance, physicalDevice);
	}

	if constexpr (GRAPHICS_VALIDATION_LEVEL > 0)
	{
		auto vkCreateDebugUtilsMessengerEXT = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
			vkGetInstanceProcAddr(
				myInstance,
				"vkCreateDebugUtilsMessengerEXT"));
		ASSERT(vkCreateDebugUtilsMessengerEXT != nullptr);

		VK_CHECK(vkCreateDebugUtilsMessengerEXT(
			myInstance,
			&gDebugUtilsMessengerCallbackCreateInfo,
			&myHostAllocationCallbacks,
			&gdebugUtilsMessenger));
	}
}

template <>
Instance<kVk>::~Instance()
{
	using namespace instance;

	ZoneScopedN("~Instance()");

	if constexpr (GRAPHICS_VALIDATION_LEVEL > 0)
	{
		auto vkDestroyDebugUtilsMessengerEXT = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
			vkGetInstanceProcAddr(
				myInstance,
				"vkDestroyDebugUtilsMessengerEXT"));
		ASSERT(vkDestroyDebugUtilsMessengerEXT != nullptr);

		vkDestroyDebugUtilsMessengerEXT(
			myInstance, gdebugUtilsMessenger, &GetHostAllocationCallbacks());
	}

	vkDestroyInstance(myInstance, &myHostAllocationCallbacks);
}
