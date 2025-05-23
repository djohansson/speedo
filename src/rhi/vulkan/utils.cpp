#include "utils.h"

#include <core/assert.h>

#define VMA_IMPLEMENTATION
#define VMA_ASSERT(expr) ASSERT(expr)
#if (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 1)
#	define VMA_DEBUG_INITIALIZE_ALLOCATIONS 1
#	define VMA_DEBUG_DETECT_CORRUPTION 1
#	define VMA_DEBUG_MARGIN 16
//#	define VMA_DEBUG_LOG_FORMAT(format, ...) LOG_ERROR(format, __VA_ARGS__) // uncomment this to get leak reports and other verbose information
#endif
#if defined(__WINDOWS__)
#	include <vma/vk_mem_alloc.h>
#else
#	include <vk_mem_alloc.h>
#endif

#include <GLFW/glfw3.h>
#if __WINDOWS__
#	define GLFW_EXPOSE_NATIVE_WIN32
#	include <GLFW/glfw3native.h>
#endif

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <print>

PFN_vkGetPhysicalDeviceFeatures2 gVkGetPhysicalDeviceFeatures2{};
PFN_vkGetPhysicalDeviceProperties2 gVkGetPhysicalDeviceProperties2{};
PFN_vkWaitForPresentKHR gVkWaitForPresentKHR{};
PFN_vkGetBufferMemoryRequirements2KHR gVkGetBufferMemoryRequirements2KHR{};
PFN_vkGetImageMemoryRequirements2KHR gVkGetImageMemoryRequirements2KHR{};
PFN_vkCmdBeginRenderingKHR gVkCmdBeginRenderingKHR{};
PFN_vkCmdEndRenderingKHR gVkCmdEndRenderingKHR{};
#if (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
PFN_vkCreateDebugUtilsMessengerEXT gVkCreateDebugUtilsMessengerEXT{};
PFN_vkDestroyDebugUtilsMessengerEXT gVkDestroyDebugUtilsMessengerEXT{};
PFN_vkSetDebugUtilsObjectNameEXT gVkSetDebugUtilsObjectNameExt{};
#endif
PFN_vkCmdSetCheckpointNV gVkCmdSetCheckpointNV{};
PFN_vkGetQueueCheckpointData2NV gVkGetQueueCheckpointData2NV{};
PFN_vkCmdPipelineBarrier2KHR gVkCmdPipelineBarrier2KHR{};
PFN_vkCmdPushDescriptorSetWithTemplateKHR gVkCmdPushDescriptorSetWithTemplateKHR{};

void InitInstanceExtensions(VkInstance instance)
{
	if (gVkGetPhysicalDeviceFeatures2 == nullptr)
		gVkGetPhysicalDeviceFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
			vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceFeatures2"));
	
	ENSURE(gVkGetPhysicalDeviceFeatures2 != nullptr);

	if (gVkGetPhysicalDeviceProperties2 == nullptr)
		gVkGetPhysicalDeviceProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
			vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties2"));

	ENSURE(gVkGetPhysicalDeviceProperties2 != nullptr);

	if (gVkWaitForPresentKHR == nullptr)
		gVkWaitForPresentKHR = reinterpret_cast<PFN_vkWaitForPresentKHR>(
			vkGetInstanceProcAddr(instance,"vkWaitForPresentKHR"));
	
	ENSURE(gVkWaitForPresentKHR != nullptr);

	if (gVkGetBufferMemoryRequirements2KHR == nullptr)
		gVkGetBufferMemoryRequirements2KHR = reinterpret_cast<PFN_vkGetBufferMemoryRequirements2KHR>(
			vkGetInstanceProcAddr(instance, "vkGetBufferMemoryRequirements2KHR"));
	
	ENSURE(gVkGetBufferMemoryRequirements2KHR != nullptr);

	if (gVkGetImageMemoryRequirements2KHR == nullptr)
		gVkGetImageMemoryRequirements2KHR = reinterpret_cast<PFN_vkGetImageMemoryRequirements2KHR>(
			vkGetInstanceProcAddr(instance, "vkGetImageMemoryRequirements2KHR"));

	ENSURE(gVkGetImageMemoryRequirements2KHR != nullptr);

	if (gVkCmdBeginRenderingKHR == nullptr)
		gVkCmdBeginRenderingKHR = reinterpret_cast<PFN_vkCmdBeginRenderingKHR>(
			vkGetInstanceProcAddr(instance,	"vkCmdBeginRenderingKHR"));

	ENSURE(gVkCmdBeginRenderingKHR != nullptr);

	if (gVkCmdEndRenderingKHR == nullptr)
		gVkCmdEndRenderingKHR = reinterpret_cast<PFN_vkCmdEndRenderingKHR>(
			vkGetInstanceProcAddr(instance,"vkCmdEndRenderingKHR"));

	ENSURE(gVkCmdEndRenderingKHR != nullptr);

#if (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
	//if constexpr (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
	{
		if (gVkCreateDebugUtilsMessengerEXT == nullptr)
			gVkCreateDebugUtilsMessengerEXT = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
				vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
		
		ENSURE(gVkCreateDebugUtilsMessengerEXT != nullptr);

		if (gVkDestroyDebugUtilsMessengerEXT == nullptr)
			gVkDestroyDebugUtilsMessengerEXT = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
				vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
		
		ENSURE(gVkDestroyDebugUtilsMessengerEXT != nullptr);

		if (gVkSetDebugUtilsObjectNameExt == nullptr)
			gVkSetDebugUtilsObjectNameExt = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
				vkGetInstanceProcAddr(instance, "vkSetDebugUtilsObjectNameEXT"));
		
		ENSURE(gVkSetDebugUtilsObjectNameExt != nullptr);
	}
#endif
}

void InitDeviceExtensions(VkDevice device)
{
	if (gVkCmdPipelineBarrier2KHR == nullptr)
		gVkCmdPipelineBarrier2KHR = reinterpret_cast<PFN_vkCmdPipelineBarrier2KHR>(
			vkGetDeviceProcAddr(device, "vkCmdPipelineBarrier2KHR"));
	
	ENSURE(gVkCmdPipelineBarrier2KHR != nullptr);

	if (gVkCmdPushDescriptorSetWithTemplateKHR == nullptr)
		gVkCmdPushDescriptorSetWithTemplateKHR = reinterpret_cast<PFN_vkCmdPushDescriptorSetWithTemplateKHR>(
			vkGetDeviceProcAddr(device, "vkCmdPushDescriptorSetWithTemplateKHR"));
		
	//ENSURE(gVkCmdPushDescriptorSetWithTemplateKHR != nullptr);

#if (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
	{
		if (gVkSetDebugUtilsObjectNameExt == nullptr)
			gVkSetDebugUtilsObjectNameExt = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
				vkGetDeviceProcAddr(device, "vkSetDebugUtilsObjectNameEXT"));
		
		ENSURE(gVkSetDebugUtilsObjectNameExt != nullptr);
	}
#endif

	if (gVkCmdSetCheckpointNV == nullptr)
		gVkCmdSetCheckpointNV = reinterpret_cast<PFN_vkCmdSetCheckpointNV>(
			vkGetDeviceProcAddr(device, "vkCmdSetCheckpointNV"));

	//ENSURE(gVkCmdSetCheckpointNV != nullptr);

	if (gVkGetQueueCheckpointData2NV == nullptr)
		gVkGetQueueCheckpointData2NV = reinterpret_cast<PFN_vkGetQueueCheckpointData2NV>(
			vkGetDeviceProcAddr(device, "vkGetQueueCheckpointData2NV"));

	//ENSURE(gVkGetQueueCheckpointData2NV != nullptr);
}

bool SupportsExtension(const char* extensionName, VkPhysicalDevice device)
{
	static bool gDeviceExtensionsInitialized = false;
	static UnorderedMap<VkPhysicalDevice, std::vector<VkExtensionProperties>> gDeviceExtensions;
	if (!gDeviceExtensionsInitialized)
	{
		gDeviceExtensionsInitialized = true;

		uint32_t deviceExtensionCount = 0;
		vkEnumerateDeviceExtensionProperties(device, nullptr, &deviceExtensionCount, nullptr);

		gDeviceExtensions[device].resize(deviceExtensionCount);
		vkEnumerateDeviceExtensionProperties(
			device, nullptr, &deviceExtensionCount, gDeviceExtensions[device].data());

		std::sort(
			gDeviceExtensions[device].begin(),
			gDeviceExtensions[device].end(),
			[](const VkExtensionProperties& lhs, const VkExtensionProperties& rhs) { return strcmp(lhs.extensionName, rhs.extensionName) < 0; });

		if constexpr (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
		{
			std::cout << gDeviceExtensions[device].size() << " vulkan device extension(s) found:" << '\n';
			std::for_each(
				gDeviceExtensions[device].cbegin(),
				gDeviceExtensions[device].cend(),
				[](const VkExtensionProperties& instanceExtension) {
					std::cout << instanceExtension.extensionName << '\n';
				});
		}
	}

	return std::find_if(gDeviceExtensions[device].cbegin(), gDeviceExtensions[device].cend(),
		[extensionName](const auto& extension)
		{
			return strcmp(extensionName, extension.extensionName) == 0;
		}) != gDeviceExtensions[device].end();
}

bool SupportsExtension(const char* extensionName, VkInstance instance)
{
	static bool gInstanceExtensionsInitialized = false;
	static UnorderedMap<VkInstance, std::vector<VkExtensionProperties>> gInstanceExtensions;
	if (!gInstanceExtensionsInitialized)
	{
		gInstanceExtensionsInitialized = true;
		
		uint32_t instanceExtensionCount = 0;
		vkEnumerateInstanceExtensionProperties(nullptr, &instanceExtensionCount, nullptr);

		gInstanceExtensions[instance].resize(instanceExtensionCount);
		vkEnumerateInstanceExtensionProperties(
			nullptr, &instanceExtensionCount, gInstanceExtensions[instance].data());

		std::sort(
			gInstanceExtensions[instance].begin(),
			gInstanceExtensions[instance].end(),
			[](const VkExtensionProperties& lhs, const VkExtensionProperties& rhs) { return strcmp(lhs.extensionName, rhs.extensionName) < 0; });

		if constexpr (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
		{
			std::cout << gInstanceExtensions[instance].size() << " vulkan instance extension(s) found:" << '\n';
			std::for_each(
				gInstanceExtensions[instance].cbegin(),
				gInstanceExtensions[instance].cend(),
				[](const VkExtensionProperties& instanceExtension) {
					std::cout << instanceExtension.extensionName << '\n';
				});
		}
	}

	return std::find_if(gInstanceExtensions[instance].cbegin(), gInstanceExtensions[instance].cend(),
		[extensionName](const auto& extension)
		{
			return strcmp(extensionName, extension.extensionName) == 0;
		}) != gInstanceExtensions[instance].end();
}

uint32_t GetFormatSize(VkFormat format, uint32_t& outDivisor)
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
	case VK_FORMAT_R32G32B32A32_SFLOAT:
		return 16;
	default:
		ASSERT(false); // please implement me.
		return 0;
	};
}

uint32_t GetFormatSize(VkFormat format)
{
	uint32_t unused;
	return GetFormatSize(format, unused);
}

bool HasColorComponent(VkFormat format)
{
	return !HasDepthComponent(format) && !HasStencilComponent(format);
}

bool HasStencilComponent(VkFormat format)
{
	switch (format)
	{
	case VK_FORMAT_S8_UINT:
	case VK_FORMAT_D16_UNORM_S8_UINT:
	case VK_FORMAT_D24_UNORM_S8_UINT:
	case VK_FORMAT_D32_SFLOAT_S8_UINT:
		return true;
	default:
		return false;
	}
}

bool HasDepthComponent(VkFormat format)
{
	switch (format)
	{
	case VK_FORMAT_D16_UNORM:
	case VK_FORMAT_D32_SFLOAT:
	case VK_FORMAT_D16_UNORM_S8_UINT:
	case VK_FORMAT_X8_D24_UNORM_PACK32:
	case VK_FORMAT_D24_UNORM_S8_UINT:
	case VK_FORMAT_D32_SFLOAT_S8_UINT:
		return true;
	default:
		return false;
	}
}

uint32_t
FindMemoryType(VkPhysicalDevice device, uint32_t typeFilter, VkMemoryPropertyFlags properties)
{
	VkPhysicalDeviceMemoryProperties memProperties;
	vkGetPhysicalDeviceMemoryProperties(device, &memProperties);

	for (uint32_t i = 0UL; i < memProperties.memoryTypeCount; i++)
		if (((typeFilter & (1 << i)) != 0u) &&
			((memProperties.memoryTypes[i].propertyFlags & properties) != 0u))
			return i;

	return 0;
}

VkFormat FindSupportedFormat(
	VkPhysicalDevice device,
	std::span<const VkFormat> candidates,
	VkImageTiling tiling,
	VkFormatFeatureFlags features)
{
	for (VkFormat format : candidates)
	{
		VkFormatProperties props;
		vkGetPhysicalDeviceFormatProperties(device, format, &props);

		if (tiling == VK_IMAGE_TILING_LINEAR && (props.linearTilingFeatures & features) == features)
			return format;
		if (tiling == VK_IMAGE_TILING_OPTIMAL &&
			(props.optimalTilingFeatures & features) == features)
			return format;
	}

	return VK_FORMAT_UNDEFINED;
}

void CopyBuffer(
	VkCommandBuffer commandBuffer, VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size)
{
	VkBufferCopy copyRegion{};
	copyRegion.srcOffset = 0ULL;
	copyRegion.dstOffset = 0ULL;
	copyRegion.size = size;
	vkCmdCopyBuffer(commandBuffer, srcBuffer, dstBuffer, 1, &copyRegion);
}

std::tuple<VkBuffer, VmaAllocation> CreateBuffer(
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
	allocInfo.usage = ((flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0U)
						  ? VMA_MEMORY_USAGE_GPU_ONLY
						  : VMA_MEMORY_USAGE_UNKNOWN;
	allocInfo.requiredFlags = flags;
	allocInfo.memoryTypeBits = 0UL; // memRequirements.memoryTypeBits;
	allocInfo.pUserData = (void*)debugName;

	VkBuffer outBuffer;
	VmaAllocation outBufferMemory;
	VK_ENSURE(
		vmaCreateBuffer(allocator, &bufferInfo, &allocInfo, &outBuffer, &outBufferMemory, nullptr));

	return std::make_tuple(outBuffer, outBufferMemory);
}

std::tuple<VkBuffer, VmaAllocation> CreateBuffer(
	VkCommandBuffer commandBuffer,
	VmaAllocator allocator,
	VkBuffer stagingBuffer,
	VkDeviceSize bufferSize,
	VkBufferUsageFlags usage,
	VkMemoryPropertyFlags memoryFlags,
	const char* debugName)
{
	ASSERT(bufferSize > 0);

	VkBuffer outBuffer;
	VmaAllocation outBufferMemory;

	if (stagingBuffer != nullptr)
	{
		std::tie(outBuffer, outBufferMemory) = CreateBuffer(
			allocator,
			bufferSize,
			usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			memoryFlags,
			debugName);

		CopyBuffer(commandBuffer, stagingBuffer, outBuffer, bufferSize);
	}
	else
	{
		std::tie(outBuffer, outBufferMemory) =
			CreateBuffer(allocator, bufferSize, usage, memoryFlags, debugName);
	}

	return std::make_tuple(outBuffer, outBufferMemory);
}

std::tuple<VkBuffer, VmaAllocation> CreateStagingBuffer(
	VmaAllocator allocator, const void* srcData, size_t srcDataSize, const char* debugName)
{
	auto bufferData = CreateBuffer(
		allocator,
		srcDataSize,
		VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		debugName);

	auto& [bufferHandle, memoryHandle] = bufferData;

	void* data;
	VK_ENSURE(vmaMapMemory(allocator, memoryHandle, &data));
	memcpy(data, srcData, srcDataSize);
	vmaUnmapMemory(allocator, memoryHandle);

	return bufferData;
}

void SetDefaultAccessAndStageMasks(VkImageLayout layout, VkAccessFlags2KHR& outAccessMask, VkPipelineStageFlags2KHR& outStageMask)
{
	switch (layout)
	{
	case VK_IMAGE_LAYOUT_GENERAL:
		outAccessMask = 
			VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT |
			VK_ACCESS_2_INDEX_READ_BIT |
			VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT |
			VK_ACCESS_2_UNIFORM_READ_BIT |
			VK_ACCESS_2_INPUT_ATTACHMENT_READ_BIT |
			VK_ACCESS_2_SHADER_READ_BIT |
			VK_ACCESS_2_SHADER_WRITE_BIT |
			VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
			VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
			VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
			VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
			VK_ACCESS_2_TRANSFER_READ_BIT |
			VK_ACCESS_2_TRANSFER_WRITE_BIT |
			VK_ACCESS_2_HOST_READ_BIT |
			VK_ACCESS_2_HOST_WRITE_BIT;
		outStageMask =
			VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT |
			VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
		break;
	case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
		outAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;;
		outStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
		break;
	case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
		outAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		outStageMask = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
		break;
	case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
		outAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
		outStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
		break;
	case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
		outAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
		outStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
			// VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT | // todo: detect platform support
			// VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT |
			// VK_PIPELINE_STAGE_2_GEOMETRY_SHADER_BIT |
			VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
			VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
		break;
	case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
		outAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
		outStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
		break;
	case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
		outAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
		outStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
		break;
	// case VK_IMAGE_LAYOUT_PREINITIALIZED:
	// 	outAccessMask = ;
	// 	outStageMask = ;
	// 	break;
	// case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL:
	// 	outAccessMask = ;
	// 	outStageMask = ;
	// 	break;
	// case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL:
	// 	outAccessMask = ;
	// 	outStageMask = ;
	// 	break;
	// case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:
	// 	outAccessMask = ;
	// 	outStageMask = ;
	// 	break;
	// case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL:
	// 	outAccessMask = ;
	// 	outStageMask = ;
	// 	break;
	// case VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL:
	// 	outAccessMask = ;
	// 	outStageMask = ;
	// 	break;
	// case VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL:
	// 	outAccessMask = ;
	// 	outStageMask = ;
	// 	break;
	// case VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL:
	// 	outAccessMask = ;
	// 	outStageMask = ;
	// 	break;
	// case VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL:
	// 	outAccessMask = ;
	// 	outStageMask = ;
	// 	break;
	// case VK_IMAGE_LAYOUT_RENDERING_LOCAL_READ:
	// 	outAccessMask = ;
	// 	outStageMask = ;
	// 	break;
	case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
	case VK_IMAGE_LAYOUT_UNDEFINED:
		outAccessMask = VK_ACCESS_2_NONE;
		outStageMask = VK_PIPELINE_STAGE_2_NONE;
		break;
	default:
		ASSERT(false); // not implemented yet
	}
}

void TransitionImageLayout(
	VkCommandBuffer commandBuffer,
	VkImage image,
	VkFormat format,
	VkImageLayout oldLayout,
	VkImageLayout newLayout,
	uint32_t mipLevels,
	VkImageAspectFlags aspectFlags)
{
	VkImageMemoryBarrier2KHR imageBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};

	SetDefaultAccessAndStageMasks(oldLayout, imageBarrier.srcAccessMask, imageBarrier.srcStageMask);
	SetDefaultAccessAndStageMasks(newLayout, imageBarrier.dstAccessMask, imageBarrier.dstStageMask);

	imageBarrier.oldLayout = oldLayout;
	imageBarrier.newLayout = newLayout;
	imageBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	imageBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	imageBarrier.image = image;
	imageBarrier.subresourceRange.aspectMask = aspectFlags;
	imageBarrier.subresourceRange.baseMipLevel = 0UL;
	imageBarrier.subresourceRange.levelCount = mipLevels;
	imageBarrier.subresourceRange.baseArrayLayer = 0UL;
	imageBarrier.subresourceRange.layerCount = 1;

	VkDependencyInfoKHR dependencyInfo{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
	dependencyInfo.imageMemoryBarrierCount = 1;
	dependencyInfo.pImageMemoryBarriers = &imageBarrier;

	gVkCmdPipelineBarrier2KHR(commandBuffer, &dependencyInfo);
}

void CopyBufferToImage(
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
	for (uint32_t mipIt = 0UL; mipIt < mipLevels; mipIt++)
	{
		uint32_t mipWidth = width >> mipIt;
		uint32_t mipHeight = height >> mipIt;

		auto& region = regions[mipIt];
		region.bufferOffset = *(mipOffsets + static_cast<size_t>(mipIt * mipOffsetsStride));
		region.bufferRowLength = 0UL;
		region.bufferImageHeight = 0UL;
		region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.imageSubresource.mipLevel = mipIt;
		region.imageSubresource.baseArrayLayer = 0UL;
		region.imageSubresource.layerCount = 1;
		region.imageOffset = {.x = 0, .y = 0, .z = 0};
		region.imageExtent = {.width = mipWidth, .height = mipHeight, .depth = 1};
	}

	vkCmdCopyBufferToImage(
		commandBuffer,
		buffer,
		image,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		regions.size(),
		regions.data());
}

std::tuple<VkImage, VmaAllocation> CreateImage2D(
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
	allocInfo.usage = ((memoryFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0u)
						  ? VMA_MEMORY_USAGE_GPU_ONLY
						  : VMA_MEMORY_USAGE_UNKNOWN;
	allocInfo.requiredFlags = memoryFlags;
	allocInfo.memoryTypeBits = 0UL; // memRequirements.memoryTypeBits;
	allocInfo.pUserData = (void*)debugName;

	VkImage outImage;
	VmaAllocation outImageMemory;
	VmaAllocationInfo outAllocInfo;
	VK_ENSURE(vmaCreateImage(
		allocator, &imageInfo, &allocInfo, &outImage, &outImageMemory, &outAllocInfo));

	return std::make_tuple(outImage, outImageMemory);
}

std::tuple<VkImage, VmaAllocation> CreateImage2D(
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
	VkImageAspectFlags aspectFlags,
	const char* debugName,
	VkImageLayout initialLayout)
{
	ASSERT(stagingBuffer);

	auto result = CreateImage2D(
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

	TransitionImageLayout(
		commandBuffer,
		outImage,
		format,
		initialLayout,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		mipLevels,
		aspectFlags);

	CopyBufferToImage(
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

VkImageView CreateImageView2D(
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
	viewInfo.subresourceRange.baseMipLevel = 0UL;
	viewInfo.subresourceRange.levelCount = mipLevels;
	viewInfo.subresourceRange.baseArrayLayer = 0UL;
	viewInfo.subresourceRange.layerCount = 1;
	viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
	viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
	viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
	viewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;

	VkImageView outImageView;
	VK_ENSURE(vkCreateImageView(device, &viewInfo, hostAllocationCallbacks, &outImageView));

	return outImageView;
}

VkFramebuffer CreateFramebuffer(
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
	VK_ENSURE(vkCreateFramebuffer(device, &info, hostAllocator, &outFramebuffer));

	return outFramebuffer;
}

VkRenderPass CreateRenderPass(
	VkDevice device,
	const VkAllocationCallbacks* hostAllocator,
	std::span<const VkAttachmentDescription2> attachments,
	std::span<const VkSubpassDescription2> subpasses,
	std::span<const VkSubpassDependency2> subpassDependencies)
{
	VkRenderPassCreateInfo2 renderInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2};
	renderInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
	renderInfo.pAttachments = attachments.data();
	renderInfo.subpassCount = static_cast<uint32_t>(subpasses.size());
	renderInfo.pSubpasses = subpasses.data();
	renderInfo.dependencyCount = static_cast<uint32_t>(subpassDependencies.size());
	renderInfo.pDependencies = subpassDependencies.data();

	VkRenderPass outRenderPass;
	VK_ENSURE(vkCreateRenderPass2(device, &renderInfo, hostAllocator, &outRenderPass));

	return outRenderPass;
}

VkRenderPass CreateRenderPass(
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
	std::vector<VkAttachmentDescription2> attachments;

	VkAttachmentDescription2& colorAttachment = attachments.emplace_back();
	colorAttachment.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
	colorAttachment.format = colorFormat;
	colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
	colorAttachment.loadOp = colorLoadOp;
	colorAttachment.storeOp = colorStoreOp;
	colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	colorAttachment.initialLayout = colorInitialLayout;
	colorAttachment.finalLayout = colorFinalLayout;

	VkAttachmentReference2 colorAttachmentRef{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2};
	colorAttachmentRef.attachment = 0UL;
	colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	colorAttachmentRef.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;

	VkSubpassDescription2 subpass{VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2};
	subpass.pipelineBindPoint = bindPoint;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &colorAttachmentRef;

	VkAttachmentReference2 depthAttachmentRef{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2};
	if (depthFormat != VK_FORMAT_UNDEFINED)
	{
		VkAttachmentDescription2& depthAttachment = attachments.emplace_back();

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
		depthAttachmentRef.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;

		subpass.pDepthStencilAttachment = &depthAttachmentRef;
	}

	VkSubpassDependency2 dependency{VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2};
	dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
	dependency.dstSubpass = 0UL;
	dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependency.srcAccessMask = {};
	dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

	return CreateRenderPass(
		device,
		hostAllocator,
		attachments,
		{subpass},
		{dependency});
}

// todo: use callback
VkSurfaceKHR CreateSurface(VkInstance instance, const VkAllocationCallbacks* hostAllocator, WindowHandle handle)
{
	VkSurfaceKHR surface;
	VK_ENSURE(glfwCreateWindowSurface(
		instance,
		reinterpret_cast<GLFWwindow*>(handle),
		hostAllocator,
		&surface));

	return surface;
}

VkResult CheckFlipOrPresentResult(VkResult result)
{
	switch (result)
	{
	case VK_SUCCESS:
		break;
	case VK_TIMEOUT:
	case VK_NOT_READY:
	case VK_SUBOPTIMAL_KHR:
	case VK_ERROR_OUT_OF_DATE_KHR:
		std::println(stderr, "Warning: flip/present returned {}", string_VkResult(result));
		break;
	case VK_ERROR_SURFACE_LOST_KHR:
	case VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT:
		ASSERTF(false, "Error: flip/present returned {}", string_VkResult(result));
		break;
	case VK_ERROR_OUT_OF_HOST_MEMORY:
	case VK_ERROR_OUT_OF_DEVICE_MEMORY:
	case VK_ERROR_DEVICE_LOST:	
	default:
		ENSUREF(false, "Error: flip/present returned fatal error code: {}", string_VkResult(result));
	}

	return result;
}
