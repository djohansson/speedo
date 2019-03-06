#pragma once

#include "gfx-types.h"

#include <cassert>
#include <cstdint>
#include <vector>

#include <vulkan/vulkan.h>

static inline bool operator==(VkSurfaceFormatKHR lhs, const VkSurfaceFormatKHR &rhs)
{
	return lhs.format == rhs.format && lhs.colorSpace == rhs.colorSpace;
}

static inline void CHECK_VK(VkResult err)
{
	(void)err;
	assert(err == VK_SUCCESS);
}

uint32_t getFormatPixelSize(VkFormat format, uint32_t &outDivisor)
{
	outDivisor = 1;
	switch (format)
	{
	case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
	case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
	case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
	case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
		outDivisor = 2;
		return 1;
	case VK_FORMAT_R8G8B8_UNORM:
		return 3;
	case VK_FORMAT_R8G8B8A8_UNORM:
		return 4;
	default:
		assert(false);
		return 0;
	};
}

bool hasStencilComponent(VkFormat format)
{
	switch (format)
	{
	case VK_FORMAT_D32_SFLOAT_S8_UINT:
	case VK_FORMAT_D24_UNORM_S8_UINT:
		return true;
	default:
		return false;
	};
}

uint32_t findMemoryType(VkPhysicalDevice device, uint32_t typeFilter, VkMemoryPropertyFlags properties)
{
	VkPhysicalDeviceMemoryProperties memProperties;
	vkGetPhysicalDeviceMemoryProperties(device, &memProperties);

	for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++)
		if ((typeFilter & (1 << i)) &&
			(memProperties.memoryTypes[i].propertyFlags & properties))
			return i;

	return 0;
}

VkFormat findSupportedFormat(
	VkPhysicalDevice device,
	const std::vector<VkFormat> &candidates,
	VkImageTiling tiling,
	VkFormatFeatureFlags features)
{
	for (VkFormat format : candidates)
	{
		VkFormatProperties props;
		vkGetPhysicalDeviceFormatProperties(device, format, &props);

		if (tiling == VK_IMAGE_TILING_LINEAR && (props.linearTilingFeatures & features) == features)
			return format;
		else if (tiling == VK_IMAGE_TILING_OPTIMAL && (props.optimalTilingFeatures & features) == features)
			return format;
	}

	return VK_FORMAT_UNDEFINED;
}

VkCommandPool createCommandPool(VkDevice device, VkCommandPoolCreateFlags flags, int queueFamilyIndex)
{
	VkCommandPool pool;
	
	VkCommandPoolCreateInfo cmdPoolInfo = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	cmdPoolInfo.flags = flags;
	cmdPoolInfo.queueFamilyIndex = queueFamilyIndex;
	CHECK_VK(vkCreateCommandPool(device, &cmdPoolInfo, nullptr, &pool));
	
	return pool;
}

std::vector<VkCommandBuffer> allocateCommandBuffers(
	VkDevice device,
	VkCommandPool pool,
	VkCommandBufferLevel level,
	uint32_t count)
{
	std::vector<VkCommandBuffer> commandBuffers(count);

	VkCommandBufferAllocateInfo cmdInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	cmdInfo.commandPool = pool;
	cmdInfo.level = level;
	cmdInfo.commandBufferCount = count;
	CHECK_VK(vkAllocateCommandBuffers(device, &cmdInfo, commandBuffers.data()));

	return commandBuffers;
};

template <typename T>
VkDescriptorSetLayout createDescriptorSetLayout(VkDevice device, const std::vector<T> &bindings)
{
	VkDescriptorSetLayoutCreateInfo layoutInfo = {};
	layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
	layoutInfo.pBindings = static_cast<const VkDescriptorSetLayoutBinding *>(bindings.data());

	VkDescriptorSetLayout layout;
	CHECK_VK(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &layout));

	return layout;
}

template <typename T>
void allocateDescriptorSets(
	VkDevice device,
	VkDescriptorPool pool,
	const std::vector<T> &layouts,
	std::vector<VkDescriptorSet> &outDescriptorSets)
{
	VkDescriptorSetAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocInfo.descriptorPool = pool;
	allocInfo.descriptorSetCount = static_cast<uint32_t>(layouts.size());
	allocInfo.pSetLayouts = static_cast<const VkDescriptorSetLayout *>(layouts.data());

	outDescriptorSets.resize(layouts.size());
	CHECK_VK(vkAllocateDescriptorSets(device, &allocInfo, outDescriptorSets.data()));
}

VkCommandBuffer beginSingleTimeCommands(VkDevice device, VkCommandPool commandPool)
{
	VkCommandBuffer commandBuffer;

	VkCommandBufferAllocateInfo cmdInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	cmdInfo.commandPool = commandPool;
	cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cmdInfo.commandBufferCount = 1;
	CHECK_VK(vkAllocateCommandBuffers(device, &cmdInfo, &commandBuffer));

	VkCommandBufferBeginInfo beginInfo = {};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	CHECK_VK(vkBeginCommandBuffer(commandBuffer, &beginInfo));

	return commandBuffer;
}

void endSingleTimeCommands(VkDevice device, VkQueue queue, VkCommandBuffer commandBuffer, VkCommandPool commandPool)
{
	VkSubmitInfo endInfo = {};
	endInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	endInfo.commandBufferCount = 1;
	endInfo.pCommandBuffers = &commandBuffer;
	CHECK_VK(vkEndCommandBuffer(commandBuffer));
	CHECK_VK(vkQueueSubmit(queue, 1, &endInfo, VK_NULL_HANDLE));
	CHECK_VK(vkQueueWaitIdle(queue));

	vkFreeCommandBuffers(device, commandPool, 1, &commandBuffer);
}

int getSuitableSwapchainAndQueueFamilyIndex(
	VkSurfaceKHR surface,
	VkPhysicalDevice device,
	SwapchainInfo<GraphicsBackend::Vulkan>& outSwapchainInfo)
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
