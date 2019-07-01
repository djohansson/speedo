#pragma once

#include <cassert>
#include <cstdint>
#include <optional>
#include <map>
#include <vector>

#include <vulkan/vulkan.h>

#define VMA_IMPLEMENTATION
#ifdef _DEBUG
#	define VMA_DEBUG_INITIALIZE_ALLOCATIONS 1
#	define VMA_DEBUG_MARGIN 16
#	define VMA_DEBUG_DETECT_CORRUPTION 1
#endif
#include <vk_mem_alloc.h>

static inline bool operator==(VkSurfaceFormatKHR lhs, const VkSurfaceFormatKHR& rhs)
{
	return lhs.format == rhs.format && lhs.colorSpace == rhs.colorSpace;
}

static inline void CHECK_VK(VkResult err)
{
	(void)err;
	assert(err == VK_SUCCESS);
}

uint32_t getFormatSize(VkFormat format, uint32_t& outDivisor)
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
	case VK_FORMAT_R32G32_SFLOAT:
		return 8;
	case VK_FORMAT_R32G32B32_SFLOAT:
		return 12;
	default:
		assert(false); // please implement me.
		return 0;
	};
}

uint32_t getFormatSize(VkFormat format)
{
	uint32_t unused;
	return getFormatSize(format, unused);
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

uint32_t findMemoryType(VkPhysicalDevice device, uint32_t typeFilter,
						VkMemoryPropertyFlags properties)
{
	VkPhysicalDeviceMemoryProperties memProperties;
	vkGetPhysicalDeviceMemoryProperties(device, &memProperties);

	for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++)
		if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties))
			return i;

	return 0;
}

VkFormat findSupportedFormat(VkPhysicalDevice device, const std::vector<VkFormat>& candidates,
							 VkImageTiling tiling, VkFormatFeatureFlags features)
{
	for (VkFormat format : candidates)
	{
		VkFormatProperties props;
		vkGetPhysicalDeviceFormatProperties(device, format, &props);

		if (tiling == VK_IMAGE_TILING_LINEAR && (props.linearTilingFeatures & features) == features)
			return format;
		else if (tiling == VK_IMAGE_TILING_OPTIMAL &&
				 (props.optimalTilingFeatures & features) == features)
			return format;
	}

	return VK_FORMAT_UNDEFINED;
}

VkCommandPool createCommandPool(VkDevice device, VkCommandPoolCreateFlags flags,
								int queueFamilyIndex)
{
	VkCommandPool pool;

	VkCommandPoolCreateInfo cmdPoolInfo = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	cmdPoolInfo.flags = flags;
	cmdPoolInfo.queueFamilyIndex = queueFamilyIndex;
	CHECK_VK(vkCreateCommandPool(device, &cmdPoolInfo, nullptr, &pool));

	return pool;
}

std::vector<VkCommandBuffer> allocateCommandBuffers(VkDevice device, VkCommandPool pool,
													VkCommandBufferLevel level, uint32_t count)
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
VkDescriptorSetLayout createDescriptorSetLayout(VkDevice device, const std::vector<T>& bindings)
{
	VkDescriptorSetLayoutCreateInfo layoutInfo = {};
	layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
	layoutInfo.pBindings = static_cast<const VkDescriptorSetLayoutBinding*>(bindings.data());

	VkDescriptorSetLayout layout;
	CHECK_VK(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &layout));

	return layout;
}

template <typename T>
std::vector<VkDescriptorSet> allocateDescriptorSets(VkDevice device, VkDescriptorPool pool,
													const T* layouts, size_t layoutCount)
{
	VkDescriptorSetAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocInfo.descriptorPool = pool;
	allocInfo.descriptorSetCount = static_cast<uint32_t>(layoutCount);
	allocInfo.pSetLayouts = static_cast<const VkDescriptorSetLayout*>(layouts);

	std::vector<VkDescriptorSet> outDescriptorSets(layoutCount);
	CHECK_VK(vkAllocateDescriptorSets(device, &allocInfo, outDescriptorSets.data()));

	return outDescriptorSets;
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

void endSingleTimeCommands(VkDevice device, VkQueue queue, VkCommandBuffer commandBuffer,
						   VkCommandPool commandPool)
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

VkDebugReportCallbackEXT createDebugCallback(VkInstance instance)
{
	VkDebugReportCallbackCreateInfoEXT debugCallbackInfo = {};
	debugCallbackInfo.sType = VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT;
	debugCallbackInfo.flags =
		/* VK_DEBUG_REPORT_INFORMATION_BIT_EXT |*/ VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT |
		VK_DEBUG_REPORT_DEBUG_BIT_EXT | VK_DEBUG_REPORT_ERROR_BIT_EXT |
		VK_DEBUG_REPORT_WARNING_BIT_EXT;

	debugCallbackInfo.pfnCallback = static_cast<PFN_vkDebugReportCallbackEXT>(
		[](VkDebugReportFlagsEXT flags, VkDebugReportObjectTypeEXT /*objectType*/, uint64_t /*object*/,
		   size_t /*location*/, int32_t /*messageCode*/, const char* layerPrefix, const char* message,
		   void* /*userData*/) -> VkBool32 {
			std::cout << layerPrefix << ": " << message << std::endl;

			// VK_DEBUG_REPORT_INFORMATION_BIT_EXT = 0x00000001,
			// VK_DEBUG_REPORT_WARNING_BIT_EXT = 0x00000002,
			// VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT = 0x00000004,
			// VK_DEBUG_REPORT_ERROR_BIT_EXT = 0x00000008,
			// VK_DEBUG_REPORT_DEBUG_BIT_EXT = 0x00000010,

			if (flags & VK_DEBUG_REPORT_ERROR_BIT_EXT || flags & VK_DEBUG_REPORT_WARNING_BIT_EXT)
				return VK_TRUE;

			return VK_FALSE;
		});

	auto vkCreateDebugReportCallbackEXT = (PFN_vkCreateDebugReportCallbackEXT)vkGetInstanceProcAddr(
		instance, "vkCreateDebugReportCallbackEXT");
	assert(vkCreateDebugReportCallbackEXT != nullptr);

	VkDebugReportCallbackEXT debugCallback = VK_NULL_HANDLE;
	CHECK_VK(vkCreateDebugReportCallbackEXT(instance, &debugCallbackInfo, nullptr, &debugCallback));

	return debugCallback;
}

void copyBuffer(VkDevice device, VkCommandPool commandPool, VkQueue queue, VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size)
{
	auto commandBuffer = beginSingleTimeCommands(device, commandPool);

	VkBufferCopy copyRegion = {};
	copyRegion.srcOffset = 0;
	copyRegion.dstOffset = 0;
	copyRegion.size = size;
	vkCmdCopyBuffer(commandBuffer, srcBuffer, dstBuffer, 1, &copyRegion);

	endSingleTimeCommands(device, queue, commandBuffer, commandPool);
}

void createBuffer(
	VmaAllocator allocator, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags flags,
	VkBuffer& outBuffer, VmaAllocation& outBufferMemory, const char* debugName)
{
	VkBufferCreateInfo bufferInfo = {};
	bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferInfo.size = size;
	bufferInfo.usage = usage;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VmaAllocationCreateInfo allocInfo = {};
	allocInfo.flags = VMA_ALLOCATION_CREATE_USER_DATA_COPY_STRING_BIT;
	allocInfo.usage = (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? VMA_MEMORY_USAGE_GPU_ONLY
																	: VMA_MEMORY_USAGE_UNKNOWN;
	allocInfo.requiredFlags = flags;
	allocInfo.memoryTypeBits = 0; // memRequirements.memoryTypeBits;
	allocInfo.pUserData = (void*)debugName;

	CHECK_VK(vmaCreateBuffer(
		allocator, &bufferInfo, &allocInfo, &outBuffer, &outBufferMemory, nullptr));
}

template <typename T>
void createDeviceLocalBuffer(
	VkDevice device, VkCommandPool commandPool, VkQueue queue, VmaAllocator allocator,
	const T* bufferData, size_t bufferSize, VkBufferUsageFlags usage, VkBuffer& outBuffer,
	VmaAllocation& outBufferMemory, const char* debugName)
{
	assert(bufferData != nullptr);
	assert(bufferSize > 0);

	// todo: use staging buffer pool, or use scratchpad memory
	VkBuffer stagingBuffer;
	VmaAllocation stagingBufferMemory;
	std::string debugString;
	debugString.append(debugName);
	debugString.append("_staging");
	
	createBuffer(
		allocator, bufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		stagingBuffer, stagingBufferMemory, debugString.c_str());

	void* data;
	CHECK_VK(vmaMapMemory(allocator, stagingBufferMemory, &data));
	memcpy(data, bufferData, bufferSize);
	vmaUnmapMemory(allocator, stagingBufferMemory);

	createBuffer(
		allocator, bufferSize, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, outBuffer, outBufferMemory, debugName);

	copyBuffer(device, commandPool, queue, stagingBuffer, outBuffer, bufferSize);

	vmaDestroyBuffer(allocator, stagingBuffer, stagingBufferMemory);
}

