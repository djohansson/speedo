#include <rhi/instance.h>
#include <rhi/vulkan/crashreport.h>
#include <rhi/vulkan/utils.h>

#include <core/assert.h>
#include <core/profiling.h>
#include <core/utils.h>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <format>
#include <print>
#include <string_view>
#include <vector>

#if defined(SPEEDO_USE_MIMALLOC)
#include <mimalloc.h>
#elif defined(__APPLE__)
#include <malloc/malloc.h>
#elif !defined(__WINDOWS__)
#include <malloc.h>
#endif

namespace rhi
{

namespace instance
{

#if !defined(SPEEDO_USE_MIMALLOC) && !defined(__WINDOWS__)
// aligned_alloc takes only sizes that are a multiple of the alignment, and on macos no alignment below a pointer's:
// otherwise it returns null, and vkCreateInstance and vkCreateDevice failed out of host memory
static void* AlignedAllocate(size_t size, size_t alignment)
{
	alignment = std::max(alignment, sizeof(void*));
	return std::aligned_alloc(alignment, (size + alignment - 1) / alignment * alignment);
}
#endif

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

template <typename VariantContainerType>
void* GetPNextChain(const VariantContainerType& objects)
{
	auto nextObjectIt = objects.cbegin();
	const auto objectEndIt = objects.cend();
	std::ranges::for_each(objects,
		[&nextObjectIt, &objectEndIt](auto& objectVariant)
		{
			using VariantType = std::decay_t<decltype(objectVariant)>;
			std::visit(core::std_extra::overloaded{
				[&nextObjectIt, &objectEndIt](auto& object)
				{
					if (++nextObjectIt == objectEndIt) [[unlikely]]
						object.pNext = nullptr;
					else [[likely]]
						object.pNext = const_cast<void*>(static_cast<const void*>(&(*(nextObjectIt))));
				},
			}, const_cast<VariantType&>(objectVariant));
		});

	return const_cast<void*>(static_cast<const void*>(&(*objects.begin())));
};

void GetPhysicalDeviceInfo2(
	PhysicalDeviceInfo<kVk>& deviceInfo,
	InstanceHandle<kVk> instance,
	PhysicalDeviceHandle<kVk> device)
{
	auto& deviceFeatureParams = deviceInfo.deviceFeatureParams;
	deviceFeatureParams.clear();
	deviceFeatureParams.emplace(VkPhysicalDeviceDynamicRenderingFeatures
	{
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES,
	});
	deviceFeatureParams.emplace(VkPhysicalDeviceSynchronization2Features
	{
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES,
	});
	deviceFeatureParams.emplace(VkPhysicalDeviceExtendedDynamicStateFeaturesEXT
	{
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT,
	});
	// SV_DrawIndex (the vertex stage's draw index in an indirect draw)
	deviceFeatureParams.emplace(VkPhysicalDeviceShaderDrawParametersFeatures
	{
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES,
	});
	deviceFeatureParams.emplace(VkPhysicalDeviceMultiviewFeatures
	{
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES,
	});
	deviceFeatureParams.emplace(VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR
	{
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR,
	});
	if (SupportsExtension(VK_EXT_INLINE_UNIFORM_BLOCK_EXTENSION_NAME, device))
	{
		deviceFeatureParams.emplace(VkPhysicalDeviceInlineUniformBlockFeaturesEXT
		{
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INLINE_UNIFORM_BLOCK_FEATURES_EXT,
		});
	}
	if (SupportsExtension(VK_KHR_PRESENT_ID_EXTENSION_NAME, device))
	{
		deviceFeatureParams.emplace(VkPhysicalDevicePresentIdFeaturesKHR
		{
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR,
		});
	}
	if (SupportsExtension(VK_KHR_PRESENT_WAIT_EXTENSION_NAME, device))
	{
		deviceFeatureParams.emplace(VkPhysicalDevicePresentWaitFeaturesKHR
		{
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR,
		});
	}
	// crash reports' (see crashreport.h: the device enables each of these extensions where it has it)
	if (vk::crash::ChainsFeatures(VK_EXT_DEVICE_FAULT_EXTENSION_NAME, instance, device))
	{
		deviceFeatureParams.emplace(VkPhysicalDeviceFaultFeaturesEXT
		{
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT,
		});
	}
	if (vk::crash::ChainsFeatures(VK_EXT_DEVICE_ADDRESS_BINDING_REPORT_EXTENSION_NAME, instance, device))
	{
		deviceFeatureParams.emplace(VkPhysicalDeviceAddressBindingReportFeaturesEXT
		{
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ADDRESS_BINDING_REPORT_FEATURES_EXT,
		});
	}
	if (vk::crash::ChainsFeatures(VK_AMD_DEVICE_COHERENT_MEMORY_EXTENSION_NAME, instance, device))
	{
		deviceFeatureParams.emplace(VkPhysicalDeviceCoherentMemoryFeaturesAMD
		{
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COHERENT_MEMORY_FEATURES_AMD,
		});
	}
	
	deviceInfo.deviceFeatures12Ex.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
	deviceInfo.deviceFeatures12Ex.pNext = GetPNextChain(deviceFeatureParams);

	deviceInfo.deviceFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
	deviceInfo.deviceFeatures.pNext = &deviceInfo.deviceFeatures12Ex;
	
	gVkGetPhysicalDeviceFeatures2(device, &deviceInfo.deviceFeatures);

	auto& devicePropertyParams = deviceInfo.devicePropertyParams;
	devicePropertyParams.clear();
	if (SupportsExtension(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, device))
	{
		devicePropertyParams.emplace(VkPhysicalDevicePushDescriptorPropertiesKHR
		{
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR,
		});
	}

	deviceInfo.deviceProperties12Ex.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES;
	deviceInfo.deviceProperties12Ex.pNext = GetPNextChain(devicePropertyParams);
	
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
	if (pCallbackData->pMessageIdName != nullptr)
		std::print("{}: ", pCallbackData->pMessageIdName);
	if (pCallbackData->pMessage != nullptr)
		std::println("{}", pCallbackData->pMessage);

	std::fflush(stdout);

	// SPEEDO_VALIDATION_BREAK selects the message severities that break into an attached debugger:
	// "off" (the default, or "0") never breaks, "error" breaks on errors, "warning" on errors and warnings.
	// VK_DEBUG_UTILS_MESSAGE_SEVERITY flags are erroneously defined in hex instead of binary, so we cant and them together and check for combinations in any meaningful way.
	static const std::vector<VkFlags> gBreakOnSeverityCategory = []
	{
		std::vector<VkFlags> categories;
		if (const char* breakSetting = std::getenv("SPEEDO_VALIDATION_BREAK"))
		{
			std::string_view setting(breakSetting);
			if (setting == "warning")
				categories = {VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT, VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT};
			else if (setting == "error")
				categories = {VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT};
			else if (setting != "off" && setting != "0")
				std::println(stderr, "Unknown SPEEDO_VALIDATION_BREAK value '{}', expected off, error or warning", setting);
		}
		return categories;
	}();

	if (!gBreakOnSeverityCategory.empty() && std::is_debugger_present())
	{
		for (const auto& category : gBreakOnSeverityCategory)
		{
			if ((messageSeverity & category) != 0U)
			{
				DEBUGTRAP();
			}
		}
	}

	return VK_FALSE;
}

VkDebugUtilsMessengerCreateInfoEXT gDebugUtilsMessengerCallbackCreateInfo{
	.sType=VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
	.pNext=nullptr,
	.flags=0,
	.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
		VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
		VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
		VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT,
	.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
		VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
	.pfnUserCallback=DebugUtilsMessengerCallback,
	.pUserData=nullptr};

} // namespace instance

template <>
InstanceCreateDesc<kVk>::InstanceCreateDesc(std::string&& applicationName, std::string&& engineName) noexcept
	: applicationName(std::forward<std::string>(applicationName))
	, engineName(std::forward<std::string>(engineName))
	, appInfo{
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pNext = nullptr,
		.pApplicationName = nullptr,
		.applicationVersion = VK_MAKE_VERSION(1, 0, 0),
		.pEngineName = nullptr,
		.engineVersion = VK_MAKE_VERSION(1, 0, 0),
		.apiVersion = VK_API_VERSION_1_2}
{
	appInfo.pApplicationName = this->applicationName.c_str();
	appInfo.pEngineName = this->engineName.c_str();
}

template <>
InstanceCreateDesc<kVk>::InstanceCreateDesc(InstanceCreateDesc&& other) noexcept
	: applicationName(std::forward<std::string>(other.applicationName))
	, engineName(std::forward<std::string>(other.engineName))
	, appInfo(std::forward<ApplicationInfo<kVk>>(other.appInfo))
{
	appInfo.pApplicationName = applicationName.c_str();
	appInfo.pEngineName = engineName.c_str();
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
Instance<kVk>::Instance(InstanceCreateDesc<kVk>&& desc)
: myConfig(std::forward<InstanceCreateDesc<kVk>>(desc))
, myHostAllocationCallbacks{
	.pUserData = nullptr,
#if defined(SPEEDO_USE_MIMALLOC)
	.pfnAllocation = [](void* /*pUserData*/, size_t size, size_t alignment, VkSystemAllocationScope /*allocationScope*/)
	{
		return mi_malloc_aligned(size, alignment);
	},
	.pfnReallocation = [](void* /*pUserData*/, void* pOriginal, size_t size, size_t alignment, VkSystemAllocationScope /*allocationScope*/)
	{
		return mi_realloc_aligned(pOriginal, size, alignment);
	},
	.pfnFree = [](void* /*pUserData*/, void* pMemory)
	{
		mi_free(pMemory);
	},
#elif defined(__WINDOWS__)
	.pfnAllocation = [](void* /*pUserData*/, size_t size, size_t alignment, VkSystemAllocationScope /*allocationScope*/)
	{
		return _aligned_malloc(size, alignment);
	},
	.pfnReallocation = [](void* /*pUserData*/, void* pOriginal, size_t size, size_t alignment, VkSystemAllocationScope /*allocationScope*/)
	{
		return _aligned_realloc(pOriginal, size, alignment);
	},
	.pfnFree = [](void* /*pUserData*/, void* pMemory)
	{
		_aligned_free(pMemory);
	},
#else
	// realloc keeps no alignment, so a reallocation is a new allocation and a copy
	.pfnAllocation = [](void* /*pUserData*/, size_t size, size_t alignment, VkSystemAllocationScope /*allocationScope*/)
	{
		return instance::AlignedAllocate(size, alignment);
	},
	.pfnReallocation = [](void* /*pUserData*/, void* pOriginal, size_t size, size_t alignment, VkSystemAllocationScope /*allocationScope*/) -> void*
	{
		if (size == 0)
		{
			std::free(pOriginal);
			return nullptr;
		}
		void* memory = instance::AlignedAllocate(size, alignment);
		if (memory != nullptr && pOriginal != nullptr)
		{
#if defined(__APPLE__)
			std::memcpy(memory, pOriginal, std::min(size, malloc_size(pOriginal)));
#else
			std::memcpy(memory, pOriginal, std::min(size, malloc_usable_size(pOriginal)));
#endif
			std::free(pOriginal);
		}
		return memory;
	},
	.pfnFree = [](void* /*pUserData*/, void* pMemory)
	{
		std::free(pMemory);
	},
#endif
	.pfnInternalAllocation = nullptr,
	.pfnInternalFree = nullptr}
{
	using namespace instance;

	ZoneScopedN("Instance()");

	if constexpr (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
	{
		static const char* gVkLoaderDebugStr = "VK_LOADER_DEBUG";
		if (char* vkLoaderDebug = getenv(gVkLoaderDebugStr))
			std::println("{}={}", gVkLoaderDebugStr, vkLoaderDebug);

		static const char* gVkLayerPathStr = "VK_LAYER_PATH";
		if (char* vkLayerPath = getenv(gVkLayerPathStr))
			std::println("{}={}", gVkLayerPathStr, vkLayerPath);

		static const char* gVkDriverFilesStr = "VK_DRIVER_FILES";
		if (char* vkDriverFiles = getenv(gVkDriverFilesStr))
			std::println("{}={}", gVkDriverFilesStr, vkDriverFiles);
	}

	uint32_t instanceLayerCount;
	VK_CHECK(vkEnumerateInstanceLayerProperties(&instanceLayerCount, nullptr));
	
	if constexpr (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
		std::println("{} vulkan layer(s) found:", instanceLayerCount);
	
	if (instanceLayerCount > 0)
	{
		std::vector<VkLayerProperties> instanceLayers(instanceLayerCount);
		VK_CHECK(vkEnumerateInstanceLayerProperties(&instanceLayerCount, instanceLayers.data()));
		if constexpr (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
		{
			for (uint32_t i = 0; i < instanceLayerCount; ++i)
				std::println("{}", instanceLayers[i].layerName);
		}
	}

	std::fflush(stdout);

	std::vector<const char*> requiredExtensions = {
		VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
	#if defined(__OSX__)
		VK_EXT_METAL_SURFACE_EXTENSION_NAME,
		VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME,
	#elif defined(__WINDOWS__)
		"VK_KHR_win32_surface", // VK_KHR_WIN32_SURFACE_EXTENSION_NAME, without vulkan_win32.h (which requires windows.h)
	#elif defined(__LINUX__)
		VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME,
	#endif
		VK_KHR_SURFACE_EXTENSION_NAME,
		VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME,
	};
	std::vector<const char*> requiredLayers = {};

	VkInstanceCreateInfo info{.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};

	// in every build where it exists: crash reports name the objects bound around a faulting address through its messenger
	// (see crashreport.h); validation's messages need it too
	if (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0 || vk::crash::WantsDebugUtils(myInstance))
		requiredExtensions.emplace_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

	if constexpr (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
	{

		static constexpr const char* kValidationLayerName = "VK_LAYER_KHRONOS_validation";

		requiredLayers.emplace_back(kValidationLayerName);

		static constexpr VkBool32 kSettingValidateCore = VK_TRUE;
		// synchronization validation (missing or wrong barriers) is slow: on with SPEEDO_VALIDATE_SYNC set
		static const VkBool32 kSettingValidateSync = std::getenv("SPEEDO_VALIDATE_SYNC") != nullptr ? VK_TRUE : VK_FALSE;
		static constexpr VkBool32 kSettingThreadSafety = VK_TRUE;
		static constexpr std::array<const char*, 1> kSettingDebugAction = {"VK_DBG_LAYER_ACTION_LOG_MSG"};
		static constexpr std::array<const char*, 4> kSettingReportFlags = {"info", "warn", "perf", "error"};
		static constexpr VkBool32 kSettingEnableMessageLimit = VK_TRUE;
		static constexpr int32_t kSsettingDuplicateMessageLimit = 3;
		// messages that are expected and not actionable
		static constexpr std::array<const char*, 1> kSettingMessageIdFilter = {"WARNING-CreateInstance-debug-warning"};
		static constexpr size_t kLayerSettingCount = 8;

		static constexpr std::array<VkLayerSettingEXT, kLayerSettingCount> kSettings = {{
			{.pLayerName = kValidationLayerName,
			 .pSettingName = "validate_core",
			 .type = VK_LAYER_SETTING_TYPE_BOOL32_EXT,
			 .valueCount = 1,
			 .pValues = &kSettingValidateCore},
			{.pLayerName = kValidationLayerName,
			 .pSettingName = "validate_sync",
			 .type = VK_LAYER_SETTING_TYPE_BOOL32_EXT,
			 .valueCount = 1,
			 .pValues = &kSettingValidateSync},
			{.pLayerName = kValidationLayerName,
			 .pSettingName = "thread_safety",
			 .type = VK_LAYER_SETTING_TYPE_BOOL32_EXT,
			 .valueCount = 1,
			 .pValues = &kSettingThreadSafety},
			{.pLayerName = kValidationLayerName,
			 .pSettingName = "debug_action",
			 .type = VK_LAYER_SETTING_TYPE_STRING_EXT,
			 .valueCount = kSettingDebugAction.size(),
			 .pValues = kSettingDebugAction.data()},//NOLINT(bugprone-multi-level-implicit-pointer-conversion)
			{.pLayerName = kValidationLayerName,
			 .pSettingName = "report_flags",
			 .type = VK_LAYER_SETTING_TYPE_STRING_EXT,
			 .valueCount = kSettingReportFlags.size(),
			 .pValues = kSettingReportFlags.data()},//NOLINT(bugprone-multi-level-implicit-pointer-conversion)
			{.pLayerName=kValidationLayerName,
			 .pSettingName = "enable_message_limit",
			 .type = VK_LAYER_SETTING_TYPE_BOOL32_EXT,
			 .valueCount = 1,
			 .pValues = &kSettingEnableMessageLimit},
			{.pLayerName = kValidationLayerName,
			 .pSettingName = "duplicate_message_limit",
			 .type = VK_LAYER_SETTING_TYPE_UINT32_EXT,
			 .valueCount = 1,
			 .pValues = &kSsettingDuplicateMessageLimit},
			{.pLayerName = kValidationLayerName,
			 .pSettingName = "message_id_filter",
			 .type = VK_LAYER_SETTING_TYPE_STRING_EXT,
			 .valueCount = kSettingMessageIdFilter.size(),
			 .pValues = kSettingMessageIdFilter.data()}}};//NOLINT(bugprone-multi-level-implicit-pointer-conversion)

		static constexpr VkLayerSettingsCreateInfoEXT kLayerSettingsCreateInfo = {
			.sType=VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT,
			.pNext=&gDebugUtilsMessengerCallbackCreateInfo,
			.settingCount=static_cast<uint32_t>(kSettings.size()),
			.pSettings=kSettings.data()};

		info.pNext = &kLayerSettingsCreateInfo;
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

	VK_CHECK(vkCreateInstance(&info, &myHostAllocationCallbacks, &myInstance));

	InitInstanceExtensions(myInstance);

	// before any device binds memory
	if (vk::crash::WantsDebugUtils(myInstance))
		vk::crash::CreateMessenger(myInstance, &myHostAllocationCallbacks);

	// instance level objects are named once there is a device (see TrackInstanceObject)
	TrackInstance(VK_OBJECT_TYPE_INSTANCE, myInstance, std::format("{} Instance", myConfig.applicationName));

	uint32_t physicalDeviceCount = 0;
	VK_CHECK(vkEnumeratePhysicalDevices(myInstance, &physicalDeviceCount, nullptr));
	ENSUREF(physicalDeviceCount > 0, "Failed to find GPUs with Vulkan support.");
	
	myPhysicalDevices.resize(physicalDeviceCount);
	VK_CHECK(vkEnumeratePhysicalDevices(myInstance, &physicalDeviceCount, myPhysicalDevices.data()));

	for (auto* physicalDevice : myPhysicalDevices)
	{
		auto infoInsertNode = myPhysicalDeviceInfos.emplace(
			physicalDevice, std::make_unique<PhysicalDeviceInfo<kVk>>());
		GetPhysicalDeviceInfo2(*infoInsertNode.first->second, myInstance, physicalDevice);

		TrackInstance(
			VK_OBJECT_TYPE_PHYSICAL_DEVICE,
			physicalDevice,
			std::string_view(infoInsertNode.first->second->deviceProperties.properties.deviceName));
	}

#if (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
	//if constexpr (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
	{
		VK_CHECK(gVkCreateDebugUtilsMessengerEXT(
			myInstance,
			&gDebugUtilsMessengerCallbackCreateInfo,
			&myHostAllocationCallbacks,
			&gDebugUtilsMessenger));
	}
#endif
}

template <>
Instance<kVk>::~Instance()
{
	using namespace instance;

	ZoneScopedN("~Instance()");

	if (myInstance == VK_NULL_HANDLE) // e.g. the null instance GetInstance() falls back to
		return;

#if (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
	//if constexpr (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
	{
		gVkDestroyDebugUtilsMessengerEXT(
			myInstance, gDebugUtilsMessenger, &GetHostAllocationCallbacks());
	}
#endif

	vk::crash::DestroyMessenger(myInstance, &myHostAllocationCallbacks);

	for (auto* physicalDevice : myPhysicalDevices)
		Untrack(VK_OBJECT_TYPE_PHYSICAL_DEVICE, physicalDevice);

	Untrack(VK_OBJECT_TYPE_INSTANCE, myInstance);
	vkDestroyInstance(myInstance, &myHostAllocationCallbacks);
}

} // namespace rhi
