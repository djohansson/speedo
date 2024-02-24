#define VMA_IMPLEMENTATION
// #ifdef _DEBUG
// #	define VMA_DEBUG_INITIALIZE_ALLOCATIONS 1
// #	define VMA_DEBUG_DETECT_CORRUPTION 1
// #	define VMA_DEBUG_MARGIN 16
// #	define VMA_DEBUG_LOG(format, ...) do \
// 	{ \
// 		printf(format, __VA_ARGS__); \
// 		printf("\n"); \
// 	} while(false)
// #endif
#include <vma/vk_mem_alloc.h>

#include <GLFW/glfw3.h>
#if __WINDOWS__
#	define GLFW_EXPOSE_NATIVE_WIN32
#	include <GLFW/glfw3native.h>
#endif

#include "utils.h"

#include <array>
#include <iostream>

uint32_t getFormatSize(VkFormat format, uint32_t& outDivisor)
{
	outDivisor = 1;
	switch (format)
	{
	case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
	case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
	case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
	case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
	case VK_FORMAT_BC4_UNORM_BLOCK:
	case VK_FORMAT_BC4_SNORM_BLOCK:
		outDivisor = 2;
		return 1;
	case VK_FORMAT_BC2_UNORM_BLOCK:
	case VK_FORMAT_BC2_SRGB_BLOCK:
	case VK_FORMAT_BC3_UNORM_BLOCK:
	case VK_FORMAT_BC3_SRGB_BLOCK:
	case VK_FORMAT_BC5_SNORM_BLOCK:
	case VK_FORMAT_BC6H_UFLOAT_BLOCK:
	case VK_FORMAT_BC5_UNORM_BLOCK:
	case VK_FORMAT_BC6H_SFLOAT_BLOCK:
	case VK_FORMAT_BC7_UNORM_BLOCK:
	case VK_FORMAT_BC7_SRGB_BLOCK:
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
	}
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
	}
}

uint32_t
findMemoryType(VkPhysicalDevice device, uint32_t typeFilter, VkMemoryPropertyFlags properties)
{
	VkPhysicalDeviceMemoryProperties memProperties;
	vkGetPhysicalDeviceMemoryProperties(device, &memProperties);

	for (uint32_t i = 0ul; i < memProperties.memoryTypeCount; i++)
		if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties))
			return i;

	return 0;
}

VkFormat findSupportedFormat(
	VkPhysicalDevice device,
	const std::vector<VkFormat>& candidates,
	VkImageTiling tiling,
	VkFormatFeatureFlags features)
{
	for (VkFormat format : candidates)
	{
		VkFormatProperties props;
		vkGetPhysicalDeviceFormatProperties(device, format, &props);

		if (tiling == VK_IMAGE_TILING_LINEAR && (props.linearTilingFeatures & features) == features)
			return format;
		else if (
			tiling == VK_IMAGE_TILING_OPTIMAL &&
			(props.optimalTilingFeatures & features) == features)
			return format;
	}

	return VK_FORMAT_UNDEFINED;
}

std::vector<VkCommandBuffer> allocateCommandBuffers(
	VkDevice device, VkCommandPool pool, VkCommandBufferLevel level, uint32_t count)
{
	std::vector<VkCommandBuffer> commandBuffers(count);

	VkCommandBufferAllocateInfo cmdInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	cmdInfo.commandPool = pool;
	cmdInfo.level = level;
	cmdInfo.commandBufferCount = count;
	VK_CHECK(vkAllocateCommandBuffers(device, &cmdInfo, commandBuffers.data()));

	return commandBuffers;
}

VkDescriptorSet
allocateDescriptorSet(VkDevice device, VkDescriptorPool pool, VkDescriptorSetLayout layout)
{
	VkDescriptorSetAllocateInfo allocInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	allocInfo.descriptorPool = pool;
	allocInfo.descriptorSetCount = 1;
	allocInfo.pSetLayouts = &layout;

	VkDescriptorSet outDescriptorSet;
	VK_CHECK(vkAllocateDescriptorSets(device, &allocInfo, &outDescriptorSet));

	return outDescriptorSet;
}

std::vector<VkDescriptorSet> allocateDescriptorSets(
	VkDevice device,
	VkDescriptorPool pool,
	const VkDescriptorSetLayout* layouts,
	uint32_t layoutCount)
{
	VkDescriptorSetAllocateInfo allocInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	allocInfo.descriptorPool = pool;
	allocInfo.descriptorSetCount = layoutCount;
	allocInfo.pSetLayouts = layouts;

	std::vector<VkDescriptorSet> outDescriptorSets(layoutCount);
	VK_CHECK(vkAllocateDescriptorSets(device, &allocInfo, outDescriptorSets.data()));

	return outDescriptorSets;
}

VkShaderModule createShaderModule(VkDevice device, const VkAllocationCallbacks* hostAllocator, size_t codeSize, const uint32_t* codePtr)
{
	VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	info.codeSize = codeSize;
	info.pCode = codePtr;

	VkShaderModule vkShaderModule;
	VK_CHECK(vkCreateShaderModule(device, &info, hostAllocator, &vkShaderModule));

	return vkShaderModule;
};

VkDescriptorSetLayout createDescriptorSetLayout(
	VkDevice device,
	const VkAllocationCallbacks* hostAllocator,
	VkDescriptorSetLayoutCreateFlags flags,
	const VkDescriptorSetLayoutBinding* bindings,
	const VkDescriptorBindingFlags* bindingFlags,
	uint32_t bindingCount)
{
	VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsInfo{
		VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
	bindingFlagsInfo.bindingCount = bindingCount;
	bindingFlagsInfo.pBindingFlags = bindingFlags;

	VkDescriptorSetLayoutCreateInfo layoutInfo{
		VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	layoutInfo.pNext = &bindingFlagsInfo;
	layoutInfo.flags = flags;
	layoutInfo.bindingCount = bindingCount;
	layoutInfo.pBindings = bindings;

	VkDescriptorSetLayout layout;
	VK_CHECK(vkCreateDescriptorSetLayout(device, &layoutInfo, hostAllocator, &layout));

	return layout;
}

void copyBuffer(
	VkCommandBuffer commandBuffer, VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size)
{
	VkBufferCopy copyRegion{};
	copyRegion.srcOffset = 0ull;
	copyRegion.dstOffset = 0ull;
	copyRegion.size = size;
	vkCmdCopyBuffer(commandBuffer, srcBuffer, dstBuffer, 1, &copyRegion);
}

std::tuple<VkBuffer, VmaAllocation> createBuffer(
	VmaAllocator allocator,
	VkDeviceSize size,
	VkBufferUsageFlags usage,
	VkMemoryPropertyFlags flags,
	const char* debugName)
{
	VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bufferInfo.size = size;
	bufferInfo.usage = usage;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VmaAllocationCreateInfo allocInfo{};
	allocInfo.flags = VMA_ALLOCATION_CREATE_USER_DATA_COPY_STRING_BIT;
	allocInfo.usage = (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? VMA_MEMORY_USAGE_GPU_ONLY
																	: VMA_MEMORY_USAGE_UNKNOWN;
	allocInfo.requiredFlags = flags;
	allocInfo.memoryTypeBits = 0ul; // memRequirements.memoryTypeBits;
	allocInfo.pUserData = (void*)debugName;

	VkBuffer outBuffer;
	VmaAllocation outBufferMemory;
	VK_CHECK(
		vmaCreateBuffer(allocator, &bufferInfo, &allocInfo, &outBuffer, &outBufferMemory, nullptr));

	return std::make_tuple(outBuffer, outBufferMemory);
}

std::tuple<VkBuffer, VmaAllocation> createBuffer(
	VkCommandBuffer commandBuffer,
	VmaAllocator allocator,
	VkBuffer stagingBuffer,
	VkDeviceSize bufferSize,
	VkBufferUsageFlags usage,
	VkMemoryPropertyFlags memoryFlags,
	const char* debugName)
{
	assert(bufferSize > 0);

	VkBuffer outBuffer;
	VmaAllocation outBufferMemory;

	if (stagingBuffer)
	{
		std::tie(outBuffer, outBufferMemory) = createBuffer(
			allocator,
			bufferSize,
			usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			memoryFlags,
			debugName);

		copyBuffer(commandBuffer, stagingBuffer, outBuffer, bufferSize);
	}
	else
	{
		std::tie(outBuffer, outBufferMemory) =
			createBuffer(allocator, bufferSize, usage, memoryFlags, debugName);
	}

	return std::make_tuple(outBuffer, outBufferMemory);
}

std::tuple<VkBuffer, VmaAllocation> createStagingBuffer(
	VmaAllocator allocator, const void* srcData, size_t srcDataSize, const char* debugName)
{
	auto bufferData = createBuffer(
		allocator,
		srcDataSize,
		VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		debugName);

	auto& [bufferHandle, memoryHandle] = bufferData;

	void* data;
	VK_CHECK(vmaMapMemory(allocator, memoryHandle, &data));
	memcpy(data, srcData, srcDataSize);
	vmaUnmapMemory(allocator, memoryHandle);

	return bufferData;
}

void transitionImageLayout(
	VkCommandBuffer commandBuffer,
	VkImage image,
	VkFormat format,
	VkImageLayout oldLayout,
	VkImageLayout newLayout,
	uint32_t mipLevels)
{
	VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	barrier.oldLayout = oldLayout;
	barrier.newLayout = newLayout;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;

	if (hasDepthComponent(format))
	{
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;

		if (hasStencilComponent(format))
			barrier.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
	}
	else
	{
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	}

	barrier.subresourceRange.baseMipLevel = 0ul;
	barrier.subresourceRange.levelCount = mipLevels;
	barrier.subresourceRange.baseArrayLayer = 0ul;
	barrier.subresourceRange.layerCount = 1;

	VkPipelineStageFlags sourceStage{};
	VkPipelineStageFlags destinationStage{};

	if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
	{
		barrier.srcAccessMask = 0ul;
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
		destinationStage = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
						   // VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT |
						   // VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT |
						   // VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT |
						   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
						   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
	}
	else if (
		oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
		newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
	{
		barrier.srcAccessMask = 0ul;
		barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
								VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

		sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		destinationStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
	}
	else if (
		oldLayout == VK_IMAGE_LAYOUT_GENERAL &&
		newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
	{
		barrier.srcAccessMask =
			VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_INDEX_READ_BIT |
			VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT |
			VK_ACCESS_INPUT_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT |
			VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
			VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
			VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT |
			VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
								VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

		sourceStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
		destinationStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
	}
	else if (
		oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_GENERAL)
	{
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask =
			VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_INDEX_READ_BIT |
			VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT |
			VK_ACCESS_INPUT_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT |
			VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
			VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
			VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT |
			VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT;

		sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		destinationStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	}
	else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_GENERAL)
	{
		barrier.srcAccessMask = 0ul;
		barrier.dstAccessMask =
			VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_INDEX_READ_BIT |
			VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT |
			VK_ACCESS_INPUT_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT |
			VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
			VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
			VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT |
			VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT;

		sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		destinationStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	}
	else if (
		oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
		newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
	{
		barrier.srcAccessMask = 0ul;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

		sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		destinationStage = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
						   // VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT |
						   // VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT |
						   // VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT |
						   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
						   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
	}
	else if (
		oldLayout == VK_IMAGE_LAYOUT_GENERAL &&
		newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
	{
		barrier.srcAccessMask =
			VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_INDEX_READ_BIT |
			VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT |
			VK_ACCESS_INPUT_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT |
			VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
			VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
			VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT |
			VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

		sourceStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
		destinationStage = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
						   // VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT |
						   // VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT |
						   // VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT |
						   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
						   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
	}
	else if (
		oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
		newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
	{
		barrier.srcAccessMask =
			VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

		sourceStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	}
	else if (
		oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
		newLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
	{
		barrier.srcAccessMask =
			VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;

		sourceStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	}
	else if (
		oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
		newLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
	{
		barrier.srcAccessMask = 0ul;
		barrier.dstAccessMask =
			VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

		sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		destinationStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	}
	else if (
		oldLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
		newLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
	{
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		barrier.dstAccessMask =
			VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

		sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		destinationStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	}
	else if (
		oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
		newLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
	{
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask =
			VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

		sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		destinationStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	}
	else if (
		oldLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL &&
		newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
	{
		barrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
								VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

		sourceStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
		destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	}
	else if (
		oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
		newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
	{
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
								VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

		sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		destinationStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
	}
	else if (
		oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
		newLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
	{
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;

		sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	}
	else if (
		oldLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR &&
		newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
	{
		barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

		sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	}
	else
	{
		assert(false); // not implemented yet
		barrier.srcAccessMask = 0ul;
		barrier.dstAccessMask = 0ul;
	}

	vkCmdPipelineBarrier(
		commandBuffer, sourceStage, destinationStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void copyBufferToImage(
	VkCommandBuffer commandBuffer,
	VkBuffer buffer,
	VkImage image,
	uint32_t width,
	uint32_t height,
	uint32_t mipLevels,
	const uint32_t* mipOffsets,
	uint32_t mipOffsetsStride)
{
	std::vector<VkBufferImageCopy> regions(mipLevels);
	for (uint32_t mipIt = 0ul; mipIt < mipLevels; mipIt++)
	{
		uint32_t mipWidth = width >> mipIt;
		uint32_t mipHeight = height >> mipIt;

		auto& region = regions[mipIt];
		region.bufferOffset = *(mipOffsets + mipIt * mipOffsetsStride);
		region.bufferRowLength = 0ul;
		region.bufferImageHeight = 0ul;
		region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.imageSubresource.mipLevel = mipIt;
		region.imageSubresource.baseArrayLayer = 0ul;
		region.imageSubresource.layerCount = 1;
		region.imageOffset = {0, 0, 0};
		region.imageExtent = {mipWidth, mipHeight, 1};
	}

	vkCmdCopyBufferToImage(
		commandBuffer,
		buffer,
		image,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		regions.size(),
		regions.data());
}

std::tuple<VkImage, VmaAllocation> createImage2D(
	VmaAllocator allocator,
	uint32_t width,
	uint32_t height,
	uint32_t mipLevels,
	VkFormat format,
	VkImageTiling tiling,
	VkImageUsageFlags usage,
	VkMemoryPropertyFlags memoryFlags,
	const char* debugName,
	VkImageLayout initialLayout)
{
	VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.extent.width = width;
	imageInfo.extent.height = height;
	imageInfo.extent.depth = 1;
	imageInfo.mipLevels = mipLevels;
	imageInfo.arrayLayers = 1;
	imageInfo.format = format;
	imageInfo.tiling = tiling;
	imageInfo.usage = usage;
	imageInfo.initialLayout = initialLayout;
	imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.flags = {};

	VmaAllocationCreateInfo allocInfo{};
	allocInfo.flags = VMA_ALLOCATION_CREATE_USER_DATA_COPY_STRING_BIT;
	allocInfo.usage = (memoryFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
						  ? VMA_MEMORY_USAGE_GPU_ONLY
						  : VMA_MEMORY_USAGE_UNKNOWN;
	allocInfo.requiredFlags = memoryFlags;
	allocInfo.memoryTypeBits = 0ul; // memRequirements.memoryTypeBits;
	allocInfo.pUserData = (void*)debugName;

	VkImage outImage;
	VmaAllocation outImageMemory;
	VmaAllocationInfo outAllocInfo;
	VK_CHECK(vmaCreateImage(
		allocator, &imageInfo, &allocInfo, &outImage, &outImageMemory, &outAllocInfo));

	return std::make_tuple(outImage, outImageMemory);
}

std::tuple<VkImage, VmaAllocation> createImage2D(
	VkCommandBuffer commandBuffer,
	VmaAllocator allocator,
	VkBuffer stagingBuffer,
	uint32_t width,
	uint32_t height,
	uint32_t mipLevels,
	const uint32_t* mipOffsets,
	uint32_t mipOffsetsStride,
	VkFormat format,
	VkImageTiling tiling,
	VkImageUsageFlags usage,
	VkMemoryPropertyFlags memoryFlags,
	const char* debugName,
	VkImageLayout initialLayout)
{
	assert(stagingBuffer);

	auto result = createImage2D(
		allocator,
		width,
		height,
		mipLevels,
		format,
		tiling,
		usage | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
		memoryFlags,
		debugName,
		initialLayout);

	const auto& [outImage, outImageMemory] = result;

	transitionImageLayout(
		commandBuffer,
		outImage,
		format,
		initialLayout,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		mipLevels);

	copyBufferToImage(
		commandBuffer,
		stagingBuffer,
		outImage,
		width,
		height,
		mipLevels,
		mipOffsets,
		mipOffsetsStride);

	return result;
}

VkImageView createImageView2D(
	VkDevice device,
	const VkAllocationCallbacks* hostAllocationCallbacks,
	VkImageViewCreateFlags flags,
	VkImage image,
	VkFormat format,
	VkImageAspectFlags aspectFlags,
	uint32_t mipLevels)
{
	VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	viewInfo.flags = flags;
	viewInfo.image = image;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format = format;
	viewInfo.subresourceRange.aspectMask = aspectFlags;
	viewInfo.subresourceRange.baseMipLevel = 0ul;
	viewInfo.subresourceRange.levelCount = mipLevels;
	viewInfo.subresourceRange.baseArrayLayer = 0ul;
	viewInfo.subresourceRange.layerCount = 1;
	viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
	viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
	viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
	viewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;

	VkImageView outImageView;
	VK_CHECK(vkCreateImageView(device, &viewInfo, hostAllocationCallbacks, &outImageView));

	return outImageView;
}

VkFramebuffer createFramebuffer(
	VkDevice device,
	const VkAllocationCallbacks* hostAllocator,
	VkRenderPass renderPass,
	uint32_t attachmentCount,
	const VkImageView* attachments,
	uint32_t width,
	uint32_t height,
	uint32_t layers)
{
	VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
	info.renderPass = renderPass;
	info.attachmentCount = attachmentCount;
	info.pAttachments = attachments;
	info.width = width;
	info.height = height;
	info.layers = layers;

	VkFramebuffer outFramebuffer;
	VK_CHECK(vkCreateFramebuffer(device, &info, hostAllocator, &outFramebuffer));

	return outFramebuffer;
}

VkRenderPass createRenderPass(
	VkDevice device,
	const VkAllocationCallbacks* hostAllocator,
	const std::vector<VkAttachmentDescription>& attachments,
	const std::vector<VkSubpassDescription>& subpasses,
	const std::vector<VkSubpassDependency>& subpassDependencies)
{
	VkRenderPassCreateInfo renderPassInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
	renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
	renderPassInfo.pAttachments = attachments.data();
	renderPassInfo.subpassCount = static_cast<uint32_t>(subpasses.size());
	renderPassInfo.pSubpasses = subpasses.data();
	renderPassInfo.dependencyCount = static_cast<uint32_t>(subpassDependencies.size());
	renderPassInfo.pDependencies = subpassDependencies.data();

	VkRenderPass outRenderPass;
	VK_CHECK(vkCreateRenderPass(device, &renderPassInfo, hostAllocator, &outRenderPass));

	return outRenderPass;
}

VkRenderPass createRenderPass(
	VkDevice device,
	const VkAllocationCallbacks* hostAllocator,
	VkPipelineBindPoint bindPoint,
	VkFormat colorFormat,
	VkAttachmentLoadOp colorLoadOp,
	VkAttachmentStoreOp colorStoreOp,
	VkImageLayout colorInitialLayout,
	VkImageLayout colorFinalLayout,
	VkFormat depthFormat,
	VkAttachmentLoadOp depthLoadOp,
	VkAttachmentStoreOp depthStoreOp,
	VkImageLayout depthInitialLayout,
	VkImageLayout depthFinalLayout)
{
	std::vector<VkAttachmentDescription> attachments;

	VkAttachmentDescription& colorAttachment = attachments.emplace_back();
	colorAttachment.format = colorFormat;
	colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
	colorAttachment.loadOp = colorLoadOp;
	colorAttachment.storeOp = colorStoreOp;
	colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	colorAttachment.initialLayout = colorInitialLayout;
	colorAttachment.finalLayout = colorFinalLayout;

	VkAttachmentReference colorAttachmentRef{};
	colorAttachmentRef.attachment = 0ul;
	colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	VkSubpassDescription subpass{};
	subpass.pipelineBindPoint = bindPoint;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &colorAttachmentRef;

	VkAttachmentReference depthAttachmentRef{};
	if (depthFormat != VK_FORMAT_UNDEFINED)
	{
		VkAttachmentDescription& depthAttachment = attachments.emplace_back();

		depthAttachment.format = depthFormat;
		depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
		depthAttachment.loadOp = depthLoadOp;
		depthAttachment.storeOp = depthStoreOp;
		depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		depthAttachment.initialLayout = depthInitialLayout;
		depthAttachment.finalLayout = depthFinalLayout;

		depthAttachmentRef.attachment = 1;
		depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

		subpass.pDepthStencilAttachment = &depthAttachmentRef;
	}

	VkSubpassDependency dependency{};
	dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
	dependency.dstSubpass = 0ul;
	dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependency.srcAccessMask = {};
	dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

	return createRenderPass(
		device,
		hostAllocator,
		attachments,
		{subpass},
		{dependency});
}

VkSurfaceKHR createSurface(VkInstance instance, const VkAllocationCallbacks* hostAllocator, void* view)
{
	VkSurfaceKHR surface;
#ifdef __WINDOWS__
	auto vkCreateWin32SurfaceKHR = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(
		vkGetInstanceProcAddr(instance, "vkCreateWin32SurfaceKHR"));
	assert(vkCreateWin32SurfaceKHR);
	VkWin32SurfaceCreateInfoKHR surfaceCreateInfo{
		VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
	surfaceCreateInfo.hinstance = GetModuleHandle(NULL);
	surfaceCreateInfo.hwnd = *reinterpret_cast<HWND*>(view);
	VK_CHECK(vkCreateWin32SurfaceKHR(
		instance,
		&surfaceCreateInfo,
		hostAllocator,
		&surface));
#else
	VK_CHECK(
		glfwCreateWindowSurface(
			instance,
			reinterpret_cast<GLFWwindow*>(view),
			hostAllocator,
			&surface));
#endif

	return surface;
}

VkResult checkFlipOrPresentResult(VkResult result)
{
	switch (result)
	{
	case VK_SUCCESS:
		break;
	case VK_SUBOPTIMAL_KHR:
		std::clog << "warning: flip/present returned VK_SUBOPTIMAL_KHR\n";
		break;
	case VK_ERROR_OUT_OF_DATE_KHR:
		std::clog << "warning: flip/present returned VK_ERROR_OUT_OF_DATE_KHR\n";
		break;
	case VK_NOT_READY:
		std::clog << "warning: flip/present returned VK_NOT_READY\n";
		break;
	default:
		throw std::runtime_error("Invalid error code.");
	}

	return result;
}
