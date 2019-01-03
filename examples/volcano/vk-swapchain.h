#pragma once

#include <cassert>
#include <cstdint>
#include <vector>

#include <vulkan/vulkan.h>

#include <vk_mem_alloc.h>

struct SwapchainInfo
{
	VkSurfaceCapabilitiesKHR capabilities;
	std::vector<VkSurfaceFormatKHR> formats;
	std::vector<VkPresentModeKHR> presentModes;
	uint32_t width;
	uint32_t height;
};

struct Swapchain
{
	VkSwapchainKHR swapchain;
	SwapchainInfo info;

	std::vector<VkFramebuffer> myFrameBuffers;

	std::vector<VkImage> myColorImages;
	std::vector<VkImageView> myColorImageViews;

	VkImage myDepthImage = VK_NULL_HANDLE;
	VmaAllocation myDepthImageMemory = VK_NULL_HANDLE;
	VkImageView myDepthImageView = VK_NULL_HANDLE;
};

int getSuitableSwapchainAndQueueFamilyIndex(VkSurfaceKHR surface, VkPhysicalDevice device, SwapchainInfo& outSwapchainInfo)
{
	VkPhysicalDeviceProperties deviceProperties;
	vkGetPhysicalDeviceProperties(device, &deviceProperties);

	VkPhysicalDeviceFeatures deviceFeatures;
	vkGetPhysicalDeviceFeatures(device, &deviceFeatures);

	vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface, &outSwapchainInfo.capabilities);

	uint32_t formatCount;
	vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, nullptr);
	if (formatCount != 0)
	{
		outSwapchainInfo.formats.resize(formatCount);
		vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount,
			outSwapchainInfo.formats.data());
	}

	assert(!outSwapchainInfo.formats.empty());

	uint32_t presentModeCount;
	vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, nullptr);
	if (presentModeCount != 0)
	{
		outSwapchainInfo.presentModes.resize(presentModeCount);
		vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount,
			outSwapchainInfo.presentModes.data());
	}

	assert(!outSwapchainInfo.presentModes.empty());

	//if (deviceProperties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
	if (deviceProperties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
	{
		uint32_t queueFamilyCount = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);

		std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
		vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount,
			queueFamilies.data());

		for (int i = 0; i < static_cast<int>(queueFamilies.size()); i++)
		{
			const auto& queueFamily = queueFamilies[i];

			VkBool32 presentSupport = false;
			vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &presentSupport);

			if (queueFamily.queueCount > 0 && queueFamily.queueFlags & VK_QUEUE_GRAPHICS_BIT &&
				presentSupport)
			{
				return i;
			}
		}
	}

	return -1;
}
