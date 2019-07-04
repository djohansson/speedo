#include "gfx.h"

#define VMA_IMPLEMENTATION
#ifdef _DEBUG
#	define VMA_DEBUG_INITIALIZE_ALLOCATIONS 1
#	define VMA_DEBUG_MARGIN 16
#	define VMA_DEBUG_DETECT_CORRUPTION 1
#endif
#include <vk_mem_alloc.h>

#pragma pack(push, 1)
template <>
struct PipelineCacheHeader<GraphicsBackend::Vulkan>
{
	uint32_t headerLength = 0;
	uint32_t cacheHeaderVersion = 0;
	uint32_t vendorID = 0;
	uint32_t deviceID = 0;
	uint8_t pipelineCacheUUID[VK_UUID_SIZE] = {};
};
#pragma pack(pop)

template <>
bool isCacheValid(const PipelineCacheHeader<GraphicsBackend::Vulkan>* header, const PhysicalDeviceProperties<GraphicsBackend::Vulkan>& physicalDeviceProperties)
{
	return (header->headerLength > 0 &&
		header->cacheHeaderVersion == VK_PIPELINE_CACHE_HEADER_VERSION_ONE &&
		header->vendorID == physicalDeviceProperties.vendorID &&
		header->deviceID == physicalDeviceProperties.deviceID &&
		memcmp(header->pipelineCacheUUID, physicalDeviceProperties.pipelineCacheUUID, sizeof(header->pipelineCacheUUID)) == 0);
}

template <>
std::tuple<SwapchainInfo<GraphicsBackend::Vulkan>, int, PhysicalDeviceProperties<GraphicsBackend::Vulkan>>
getSuitableSwapchainAndQueueFamilyIndex(
	SurfaceHandle<GraphicsBackend::Vulkan> surface, PhysicalDeviceHandle<GraphicsBackend::Vulkan> device)
{
	SwapchainInfo<GraphicsBackend::Vulkan> swapchainInfo;

	VkPhysicalDeviceProperties deviceProperties;
	vkGetPhysicalDeviceProperties(device, &deviceProperties);

	VkPhysicalDeviceFeatures deviceFeatures;
	vkGetPhysicalDeviceFeatures(device, &deviceFeatures);

	assert(deviceFeatures.inheritedQueries);

	vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface, &swapchainInfo.capabilities);

	uint32_t formatCount;
	vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, nullptr);
	if (formatCount != 0)
	{
		swapchainInfo.formats.resize(formatCount);
		vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount,
											 swapchainInfo.formats.data());
	}

	assert(!swapchainInfo.formats.empty());

	uint32_t presentModeCount;
	vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, nullptr);
	if (presentModeCount != 0)
	{
		swapchainInfo.presentModes.resize(presentModeCount);
		vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount,
												  swapchainInfo.presentModes.data());
	}

	assert(!swapchainInfo.presentModes.empty());

	// if (deviceProperties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
	if (deviceProperties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
	{
		uint32_t queueFamilyCount = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);

		std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
		vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

		for (int i = 0; i < static_cast<int>(queueFamilies.size()); i++)
		{
			const auto& queueFamily = queueFamilies[i];

			VkBool32 presentSupport = false;
			vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &presentSupport);

			if (queueFamily.queueCount > 0 && queueFamily.queueFlags & VK_QUEUE_GRAPHICS_BIT &&
				presentSupport)
			{
				return std::make_tuple(swapchainInfo, i, deviceProperties);
			}
		}
	}

	return std::make_tuple(swapchainInfo, -1, deviceProperties);
}
