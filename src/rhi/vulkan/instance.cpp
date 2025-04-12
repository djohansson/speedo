#include "../instance.h"

#include "utils.h"

#include <core/assert.h>
#include <core/profiling.h>

#include <cstddef>
#include <vector>
#include <variant>
#include <iostream>

#if defined(SPEEDO_USE_MIMALLOC)
#include <mimalloc.h>
#endif

namespace instance
{

#if (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
static VkDebugUtilsMessengerEXT gDebugUtilsMessenger{};
#endif

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
	using PhysicalDeviceFeatures = std::variant<
		VkPhysicalDeviceInlineUniformBlockFeaturesEXT,
		VkPhysicalDeviceDynamicRenderingFeaturesKHR,
		VkPhysicalDeviceSynchronization2FeaturesKHR,
		VkPhysicalDevicePresentIdFeaturesKHR,
		VkPhysicalDevicePresentWaitFeaturesKHR>;
	static std::vector<PhysicalDeviceFeatures> gPhysicalDeviceFeatures;
	gPhysicalDeviceFeatures.clear();
	gPhysicalDeviceFeatures.emplace_back(VkPhysicalDeviceInlineUniformBlockFeaturesEXT
	{
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INLINE_UNIFORM_BLOCK_FEATURES_EXT,
		.pNext = nullptr,
		.inlineUniformBlock = VK_TRUE,
		.descriptorBindingInlineUniformBlockUpdateAfterBind = VK_TRUE,
	});
	gPhysicalDeviceFeatures.emplace_back(VkPhysicalDeviceDynamicRenderingFeaturesKHR
	{
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR,
		.pNext = nullptr,
		.dynamicRendering = VK_TRUE,
	});
	gPhysicalDeviceFeatures.emplace_back(VkPhysicalDeviceSynchronization2FeaturesKHR
	{
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_KHR,
		.pNext = nullptr,
		.synchronization2 = VK_TRUE,
	});
	if (SupportsExtension(VK_KHR_PRESENT_WAIT_EXTENSION_NAME, device))
	{
		gPhysicalDeviceFeatures.emplace_back(VkPhysicalDevicePresentIdFeaturesKHR
		{
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR,
			.pNext = nullptr,
			.presentId = VK_TRUE,
		});
		gPhysicalDeviceFeatures.emplace_back(VkPhysicalDevicePresentWaitFeaturesKHR
		{
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR,
			.pNext = nullptr,
			.presentWait = VK_TRUE,
		});
	}

	for (unsigned variantIt = 1; variantIt < gPhysicalDeviceFeatures.size(); variantIt++)
	{
		auto& prevFeatureVariant = gPhysicalDeviceFeatures[variantIt - 1];
		auto& featureVariant = gPhysicalDeviceFeatures[variantIt];
		std::visit(Overloaded{
			[&prevFeatureVariant](auto& feature)
			{
				feature.pNext = &prevFeatureVariant;
			},
		}, featureVariant);
	}
	
	deviceInfo.deviceFeatures12Ex.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
	deviceInfo.deviceFeatures12Ex.pNext = &gPhysicalDeviceFeatures.back();

	deviceInfo.deviceFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
	deviceInfo.deviceFeatures.pNext = &deviceInfo.deviceFeatures12Ex;
	
	gVkGetPhysicalDeviceFeatures2(device, &deviceInfo.deviceFeatures);

	using PhysicalDeviceProperties = std::variant<VkPhysicalDevicePushDescriptorPropertiesKHR>;
	static std::vector<PhysicalDeviceProperties> gPhysicalDeviceProperties;
	gPhysicalDeviceProperties.clear();
	if (SupportsExtension(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, device))
	{
		gPhysicalDeviceProperties.emplace_back(VkPhysicalDevicePushDescriptorPropertiesKHR
		{
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR,
			.pNext = nullptr,
		});
	}

	for (unsigned variantIt = 1; variantIt < gPhysicalDeviceProperties.size(); variantIt++)
	{
		auto& prevPropertyVariant = gPhysicalDeviceProperties[variantIt - 1];
		auto& propertyVariant = gPhysicalDeviceProperties[variantIt];
		std::visit(Overloaded{
			[&prevPropertyVariant](auto& property) { property.pNext = &prevPropertyVariant; },
		}, propertyVariant);
	}

	deviceInfo.deviceProperties12Ex.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES;
	deviceInfo.deviceProperties12Ex.pNext = &gPhysicalDeviceProperties.back();
	
	deviceInfo.deviceProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
	deviceInfo.deviceProperties.pNext = &deviceInfo.deviceProperties12Ex;

	gVkGetPhysicalDeviceProperties2(device, &deviceInfo.deviceProperties);

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

	ASSERT(messageSeverity <= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT);

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
InstanceConfiguration<kVk>::InstanceConfiguration(std::string&& applicationName, std::string&& engineName)
	: applicationName(std::forward<std::string>(applicationName))
	, engineName(std::forward<std::string>(engineName))
	, appInfo{
		VK_STRUCTURE_TYPE_APPLICATION_INFO,
		nullptr,
		nullptr,
		VK_MAKE_VERSION(1, 0, 0),
		nullptr,
		VK_MAKE_VERSION(1, 0, 0),
		VK_API_VERSION_1_2}
{
	appInfo.pApplicationName = this->applicationName.c_str();
	appInfo.pEngineName = this->engineName.c_str();
}

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
#if defined(SPEEDO_USE_MIMALLOC)
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
#elif defined(__WINDOWS__)
	[](void* /*pUserData*/, size_t size, size_t alignment, VkSystemAllocationScope /*allocationScope*/)
	{
		return _aligned_malloc(size, alignment);
	},
	[](void* /*pUserData*/, void* pOriginal, size_t size, size_t alignment, VkSystemAllocationScope /*allocationScope*/)
	{
		return _aligned_realloc(pOriginal, size, alignment);
	},
	[](void* /*pUserData*/, void* pMemory)
	{
		_aligned_free(pMemory);
	},
#else
[](void* /*pUserData*/, size_t size, size_t alignment, VkSystemAllocationScope /*allocationScope*/)
	{
		return std::aligned_alloc(alignment, size);
	},
	[](void* /*pUserData*/, void* pOriginal, size_t size, size_t alignment, VkSystemAllocationScope /*allocationScope*/)
	{
		return std::realloc(pOriginal, size);
	},
	[](void* /*pUserData*/, void* pMemory)
	{
		std::free(pMemory);
	},
#endif
	nullptr,
	nullptr}
{
	using namespace instance;

	ZoneScopedN("Instance()");

	if constexpr (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
	{
		static const char* gVkLoaderDebugStr = "VK_LOADER_DEBUG";
		if (char* vkLoaderDebug = getenv(gVkLoaderDebugStr))
			std::cout << gVkLoaderDebugStr << "=" << vkLoaderDebug << '\n';

		static const char* gVkLayerPathStr = "VK_LAYER_PATH";
		if (char* vkLayerPath = getenv(gVkLayerPathStr))
			std::cout << gVkLayerPathStr << "=" << vkLayerPath << '\n';

		static const char* gVkDriverFilesStr = "VK_DRIVER_FILES";
		if (char* vkDriverFiles = getenv(gVkDriverFilesStr))
			std::cout << gVkDriverFilesStr << "=" << vkDriverFiles << '\n';
	}

	uint32_t instanceLayerCount;
	VK_ENSURE(vkEnumerateInstanceLayerProperties(&instanceLayerCount, nullptr));
	
	if constexpr (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
		std::cout << instanceLayerCount << " vulkan layer(s) found:" << '\n';
	
	if (instanceLayerCount > 0)
	{
		std::vector<VkLayerProperties> instanceLayers(instanceLayerCount);
		VK_ENSURE(vkEnumerateInstanceLayerProperties(&instanceLayerCount, instanceLayers.data()));
		if constexpr (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
		{
			for (uint32_t i = 0; i < instanceLayerCount; ++i)
				std::cout << instanceLayers[i].layerName << '\n';
		}
	}

	// must be sorted lexicographically for std::includes to work! std::sort( instanceExtensions.begin(), instanceExtensions.end(), [](const char* lhs, const char* rhs) { return strcmp(lhs, rhs) < 0; });
	std::vector<const char*> requiredExtensions = {
	#if defined(__OSX__)
		VK_EXT_METAL_SURFACE_EXTENSION_NAME,
		VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
		VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME,
	#elif defined(__WINDOWS__)
		VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
	#elif defined(__LINUX__)
		VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME,
	#endif
		VK_KHR_SURFACE_EXTENSION_NAME
	};
	std::vector<const char*> requiredLayers = {};

	VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};

	if constexpr (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
	{
		requiredExtensions.emplace_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

		static constexpr std::string_view kValidationLayerName = "VK_LAYER_KHRONOS_validation";

		requiredLayers.emplace_back(kValidationLayerName.data());

		const VkBool32 settingValidateCore = VK_TRUE;
		const VkBool32 settingValidateSync = VK_FALSE;
		const VkBool32 settingThreadSafety = VK_TRUE;
		static constexpr std::array<const char*, 1> kSettingDebugAction = {"VK_DBG_LAYER_ACTION_LOG_MSG"};
		static constexpr std::array<const char*, 4> kSettingReportFlags = {"info", "warn", "perf", "error"};
		const VkBool32 settingEnableMessageLimit = VK_TRUE;
		const int32_t settingDuplicateMessageLimit = 3;

		const std::array<VkLayerSettingEXT, 7> settings = {{
			{.pLayerName = kValidationLayerName.data(),
			 .pSettingName = "validate_core",
			 .type = VK_LAYER_SETTING_TYPE_BOOL32_EXT,
			 .valueCount = 1,
			 .pValues = &settingValidateCore},
			{.pLayerName = kValidationLayerName.data(),
			 .pSettingName = "validate_sync",
			 .type = VK_LAYER_SETTING_TYPE_BOOL32_EXT,
			 .valueCount = 1,
			 .pValues = &settingValidateSync},
			{.pLayerName = kValidationLayerName.data(),
			 .pSettingName = "thread_safety",
			 .type = VK_LAYER_SETTING_TYPE_BOOL32_EXT,
			 .valueCount = 1,
			 .pValues = &settingThreadSafety},
			{.pLayerName = kValidationLayerName.data(),
			 .pSettingName = "debug_action",
			 .type = VK_LAYER_SETTING_TYPE_STRING_EXT,
			 .valueCount = kSettingDebugAction.size(),
			 .pValues = kSettingDebugAction.data()},//NOLINT(bugprone-multi-level-implicit-pointer-conversion)
			{.pLayerName = kValidationLayerName.data(),
			 .pSettingName = "report_flags",
			 .type = VK_LAYER_SETTING_TYPE_STRING_EXT,
			 .valueCount = kSettingReportFlags.size(),
			 .pValues = kSettingReportFlags.data()},//NOLINT(bugprone-multi-level-implicit-pointer-conversion)
			{.pLayerName=kValidationLayerName.data(),
			 .pSettingName = "enable_message_limit",
			 .type = VK_LAYER_SETTING_TYPE_BOOL32_EXT,
			 .valueCount = 1,
			 .pValues = &settingEnableMessageLimit},
			{.pLayerName = kValidationLayerName.data(),
			 .pSettingName = "duplicate_message_limit",
			 .type = VK_LAYER_SETTING_TYPE_UINT32_EXT,
			 .valueCount = 1,
			 .pValues = &settingDuplicateMessageLimit}}};

		const VkLayerSettingsCreateInfoEXT layerSettingsCreateInfo = {
			VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT,
			&gDebugUtilsMessengerCallbackCreateInfo,
			settings.size(),
			settings.data()};

		info.pNext = &layerSettingsCreateInfo;
	}

	for (const char* extensionName : requiredExtensions)
		ENSUREF(SupportsExtension(extensionName, myInstance), "Vulkan instance extension not supported: {}", extensionName);

	info.pApplicationInfo = &myConfig.appInfo;
	info.enabledLayerCount = static_cast<uint32_t>(requiredLayers.size());
	info.ppEnabledLayerNames = (info.enabledLayerCount != 0U) ? requiredLayers.data() : nullptr;
	info.enabledExtensionCount = static_cast<uint32_t>(requiredExtensions.size());
	info.ppEnabledExtensionNames =
		(info.enabledExtensionCount != 0U) ? requiredExtensions.data() : nullptr;
#if defined(__OSX__)
	info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif

	VK_ENSURE(vkCreateInstance(&info, &myHostAllocationCallbacks, &myInstance));

	InitInstanceExtensions(myInstance);

	uint32_t physicalDeviceCount = 0;
	VK_ENSURE(vkEnumeratePhysicalDevices(myInstance, &physicalDeviceCount, nullptr));
	ASSERTF(physicalDeviceCount > 0, "Failed to find GPUs with Vulkan support.");
	
	myPhysicalDevices.resize(physicalDeviceCount);
	VK_ENSURE(vkEnumeratePhysicalDevices(myInstance, &physicalDeviceCount, myPhysicalDevices.data()));

	for (auto* physicalDevice : myPhysicalDevices)
	{
		auto infoInsertNode = myPhysicalDeviceInfos.emplace(
			physicalDevice, std::make_unique<PhysicalDeviceInfo<kVk>>());
		GetPhysicalDeviceInfo2(*infoInsertNode.first->second, myInstance, physicalDevice);
	}

	if constexpr (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
	{
		VK_ENSURE(gVkCreateDebugUtilsMessengerEXT(
			myInstance,
			&gDebugUtilsMessengerCallbackCreateInfo,
			&myHostAllocationCallbacks,
			&gDebugUtilsMessenger));
	}
}

template <>
Instance<kVk>::~Instance()
{
	using namespace instance;

	ZoneScopedN("~Instance()");

	if constexpr (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
	{
		gVkDestroyDebugUtilsMessengerEXT(
			myInstance, gDebugUtilsMessenger, &GetHostAllocationCallbacks());
	}

	vkDestroyInstance(myInstance, &myHostAllocationCallbacks);
}
