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

bool hasDepthComponent(VkFormat format)
{
	switch (format)
	{
	case VK_FORMAT_D32_SFLOAT:
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

void transitionImageLayout(
	VkDevice device, VkCommandPool commandPool, VkQueue queue, 
	VkImage image, VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout)
{
	auto commandBuffer = beginSingleTimeCommands(device, commandPool);
	
	//TracyVkZone(myTracyContext, commandBuffer, "transitionImageLayout");

	VkImageMemoryBarrier barrier = {};
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.oldLayout = oldLayout;
	barrier.newLayout = newLayout;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;

	if (newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
	{
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;

		if (hasStencilComponent(format))
			barrier.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
	}
	else
	{
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	}

	barrier.subresourceRange.baseMipLevel = 0;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.baseArrayLayer = 0;
	barrier.subresourceRange.layerCount = 1;

	VkPipelineStageFlags sourceStage = 0;
	VkPipelineStageFlags destinationStage = 0;

	if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
		newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
	{
		barrier.srcAccessMask = 0;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

		sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	}
	else if (
		oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
		newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
	{
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

		sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		destinationStage =
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT; // wrong of accessed by another shader type
	}
	else if (
		oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
		newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
	{
		barrier.srcAccessMask = 0;
		barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
								VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

		sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		destinationStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
	}
	else
	{
		assert(false); // not implemented yet
		barrier.srcAccessMask = 0;
		barrier.dstAccessMask = 0;
	}

	vkCmdPipelineBarrier(
		commandBuffer, sourceStage, destinationStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);

	endSingleTimeCommands(device, queue, commandBuffer, commandPool);
}

void copyBufferToImage(
	VkDevice device, VkCommandPool commandPool, VkQueue queue, VmaAllocator allocator,
	VkBuffer buffer, VkImage image, uint32_t width, uint32_t height)
{
	auto commandBuffer = beginSingleTimeCommands(device, commandPool);

	//TracyVkZone(myTracyContext, commandBuffer, "copyBufferToImage");

	VkBufferImageCopy region = {};
	region.bufferOffset = 0;
	region.bufferRowLength = 0;
	region.bufferImageHeight = 0;
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.mipLevel = 0;
	region.imageSubresource.baseArrayLayer = 0;
	region.imageSubresource.layerCount = 1;
	region.imageOffset = {0, 0, 0};
	region.imageExtent = {width, height, 1};

	vkCmdCopyBufferToImage(
		commandBuffer, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

	endSingleTimeCommands(device, queue, commandBuffer, commandPool);
}

void createImage2D(
	VmaAllocator allocator,
	uint32_t width, uint32_t height, VkFormat format, VkImageTiling tiling,
	VkImageUsageFlags usage, VkMemoryPropertyFlags memoryFlags, VkImage& outImage,
	VmaAllocation& outImageMemory, const char* debugName)
{
	VkImageCreateInfo imageInfo = {};
	imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.extent.width = width;
	imageInfo.extent.height = height;
	imageInfo.extent.depth = 1;
	imageInfo.mipLevels = 1;
	imageInfo.arrayLayers = 1;
	imageInfo.format = format;
	imageInfo.tiling = tiling;
	imageInfo.usage = usage;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.flags = 0;

	VmaAllocationCreateInfo allocInfo = {};
	allocInfo.flags = VMA_ALLOCATION_CREATE_USER_DATA_COPY_STRING_BIT;
	allocInfo.usage = (memoryFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
							? VMA_MEMORY_USAGE_GPU_ONLY
							: VMA_MEMORY_USAGE_UNKNOWN;
	allocInfo.requiredFlags = memoryFlags;
	allocInfo.memoryTypeBits = 0; // memRequirements.memoryTypeBits;
	allocInfo.pUserData = (void*)debugName;

	VmaAllocationInfo outAllocInfo = {};
	CHECK_VK(vmaCreateImage(
		allocator, &imageInfo, &allocInfo, &outImage, &outImageMemory, &outAllocInfo));
}

template <typename T>
void createDeviceLocalImage2D(
	VkDevice device, VkCommandPool commandPool, VkQueue queue, VmaAllocator allocator,
	const T* imageData, uint32_t width, uint32_t height, VkFormat format,
	VkImageUsageFlags usage, VkImage& outImage, VmaAllocation& outImageMemory,
	const char* debugName)
{
	assert((width & 1) == 0);
	assert((height & 1) == 0);

	if (imageData != nullptr)
	{
		uint32_t pixelSizeBytesDivisor;
		uint32_t pixelSizeBytes = getFormatSize(format, pixelSizeBytesDivisor);
		VkDeviceSize imageSize = width * height * pixelSizeBytes / pixelSizeBytesDivisor;

		VkBuffer stagingBuffer;
		VmaAllocation stagingBufferMemory;
		std::string debugString;
		debugString.append(debugName);
		debugString.append("_staging");
		
		createBuffer(
			allocator, imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			stagingBuffer, stagingBufferMemory, debugString.c_str());

		void* data;
		CHECK_VK(vmaMapMemory(allocator, stagingBufferMemory, &data));
		memcpy(data, imageData, imageSize);
		vmaUnmapMemory(allocator, stagingBufferMemory);

		createImage2D(
			allocator, width, height, format, VK_IMAGE_TILING_OPTIMAL, usage | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, outImage, outImageMemory, debugName);

		transitionImageLayout(
			device, commandPool, queue, outImage, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_UNDEFINED,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		copyBufferToImage(device, commandPool, queue, allocator, stagingBuffer, outImage, width, height);
		transitionImageLayout(
			device, commandPool, queue, outImage, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

		vmaDestroyBuffer(allocator, stagingBuffer, stagingBufferMemory);
	}
	else
	{
		createImage2D(
			allocator, width, height, format, VK_IMAGE_TILING_OPTIMAL, usage,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, outImage, outImageMemory, debugName);

		transitionImageLayout(
			device, commandPool, queue, outImage, format, VK_IMAGE_LAYOUT_UNDEFINED,
			hasDepthComponent(format) ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	}
}

VkImageView createImageView2D(VkDevice device, VkImage image, VkFormat format, VkImageAspectFlags aspectFlags)
{	
	VkImageViewCreateInfo viewInfo = {};
	viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewInfo.image = image;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format = format;
	viewInfo.subresourceRange.aspectMask = aspectFlags;
	viewInfo.subresourceRange.baseMipLevel = 0;
	viewInfo.subresourceRange.levelCount = 1;
	viewInfo.subresourceRange.baseArrayLayer = 0;
	viewInfo.subresourceRange.layerCount = 1;
	viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
	viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
	viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
	viewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;

	VkImageView outImageView;
	CHECK_VK(vkCreateImageView(device, &viewInfo, nullptr, &outImageView));

	return outImageView;
}

VkSampler createTextureSampler(VkDevice device)
{
	VkSamplerCreateInfo samplerInfo = {};
	samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	samplerInfo.magFilter = VK_FILTER_LINEAR;
	samplerInfo.minFilter = VK_FILTER_LINEAR;
	samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samplerInfo.anisotropyEnable = VK_TRUE;
	samplerInfo.maxAnisotropy = 16;
	samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
	samplerInfo.unnormalizedCoordinates = VK_FALSE;
	samplerInfo.compareEnable = VK_FALSE;
	samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
	samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	samplerInfo.mipLodBias = 0.0f;
	samplerInfo.minLod = 0.0f;
	samplerInfo.maxLod = 0.0f;

	VkSampler outSampler;
	CHECK_VK(vkCreateSampler(device, &samplerInfo, nullptr, &outSampler));

	return outSampler;
}
