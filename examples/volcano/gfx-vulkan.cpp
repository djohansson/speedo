#include "gfx.h"

std::tuple<SwapchainInfo<GraphicsBackend::Vulkan>, int, PhysicalDeviceProperties<GraphicsBackend::Vulkan>> getSuitableSwapchainAndQueueFamilyIndex(
	VkSurfaceKHR surface, VkPhysicalDevice device)
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
