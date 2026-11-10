#include "utils.h"
#include "crashreport.h"

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


#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <print>
#include <string_view>

PFN_vkWaitForPresentKHR gVkWaitForPresentKHR{};
#if (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
PFN_vkCreateDebugUtilsMessengerEXT gVkCreateDebugUtilsMessengerEXT{};
PFN_vkDestroyDebugUtilsMessengerEXT gVkDestroyDebugUtilsMessengerEXT{};
PFN_vkSetDebugUtilsObjectNameEXT gVkSetDebugUtilsObjectNameExt{};
#endif
PFN_vkCmdSetCheckpointNV gVkCmdSetCheckpointNV{};
PFN_vkGetQueueCheckpointData2NV gVkGetQueueCheckpointData2NV{};
PFN_vkCreateAccelerationStructureKHR gVkCreateAccelerationStructureKHR{};
PFN_vkDestroyAccelerationStructureKHR gVkDestroyAccelerationStructureKHR{};
PFN_vkGetAccelerationStructureBuildSizesKHR gVkGetAccelerationStructureBuildSizesKHR{};
PFN_vkGetAccelerationStructureDeviceAddressKHR gVkGetAccelerationStructureDeviceAddressKHR{};
PFN_vkCmdBuildAccelerationStructuresKHR gVkCmdBuildAccelerationStructuresKHR{};
PFN_vkCmdWriteAccelerationStructuresPropertiesKHR gVkCmdWriteAccelerationStructuresPropertiesKHR{};
PFN_vkCmdCopyAccelerationStructureKHR gVkCmdCopyAccelerationStructureKHR{};

void OnCheckFailedDefault(VkResult result, uintptr_t count, ...)//NOLINT(modernize-avoid-variadic-functions)
{
	VkQueue queue = VK_NULL_HANDLE;
	if (count == 1)
	{
		va_list args;
		va_start(args, count);
		queue = va_arg(args, VkQueue);
		va_end(args);
	}

	// errors: the report, and the end of the process
	if (result < 0)
	{
		LOG_ERROR("Vulkan error: {}", string_VkResult(result));
		rhi::vk::crash::ReportFatal(result, queue);
	}

	// another result than the one expected, which isn't an error: a bug where it was checked. at most a few logged (each
	// with a stack trace), in case one repeats every frame
	static std::atomic_uint32_t gLogged{0};
	constexpr uint32_t kLogCount = 16;
	if (gLogged.fetch_add(1, std::memory_order_relaxed) < kLogCount)
		LOG_ERROR("Vulkan result: {}", string_VkResult(result));
#if (SPEEDO_PROFILING_LEVEL > 0)
	DEBUGTRAP();
#endif
}

void InitInstanceExtensions(VkInstance instance)
{
	if (gVkWaitForPresentKHR == nullptr)
		gVkWaitForPresentKHR = reinterpret_cast<PFN_vkWaitForPresentKHR>(
			vkGetInstanceProcAddr(instance,"vkWaitForPresentKHR"));
	
	ENSURE(gVkWaitForPresentKHR != nullptr);

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

	auto load = [device]<typename T>(T& function, const char* name) { function = reinterpret_cast<T>(vkGetDeviceProcAddr(device, name)); };
	load(gVkCreateAccelerationStructureKHR, "vkCreateAccelerationStructureKHR");
	load(gVkDestroyAccelerationStructureKHR, "vkDestroyAccelerationStructureKHR");
	load(gVkGetAccelerationStructureBuildSizesKHR, "vkGetAccelerationStructureBuildSizesKHR");
	load(gVkGetAccelerationStructureDeviceAddressKHR, "vkGetAccelerationStructureDeviceAddressKHR");
	load(gVkCmdBuildAccelerationStructuresKHR, "vkCmdBuildAccelerationStructuresKHR");
	load(gVkCmdWriteAccelerationStructuresPropertiesKHR, "vkCmdWriteAccelerationStructuresPropertiesKHR");
	load(gVkCmdCopyAccelerationStructureKHR, "vkCmdCopyAccelerationStructureKHR");
}

bool SupportsExtension(const char* extensionName, VkPhysicalDevice device)
{
	static core::UnorderedMap<VkPhysicalDevice, std::vector<VkExtensionProperties>> gDeviceExtensions;
	auto [deviceExtensionsIt, inserted] = gDeviceExtensions.try_emplace(device);
	auto& deviceExtensions = deviceExtensionsIt->second;
	if (inserted)
	{
		uint32_t deviceExtensionCount = 0;
		vkEnumerateDeviceExtensionProperties(device, nullptr, &deviceExtensionCount, nullptr);

		deviceExtensions.resize(deviceExtensionCount);
		vkEnumerateDeviceExtensionProperties(
			device, nullptr, &deviceExtensionCount, deviceExtensions.data());

		std::ranges::sort(
			deviceExtensions,
			[](const VkExtensionProperties& lhs, const VkExtensionProperties& rhs) { return strcmp(lhs.extensionName, rhs.extensionName) < 0; });

		if constexpr (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
		{
			std::println("{} vulkan device extension(s) found:", deviceExtensions.size());
			std::ranges::for_each(
				deviceExtensions,
				[](const VkExtensionProperties& instanceExtension) {
					std::println("{}", instanceExtension.extensionName);
				});
		}
	}

	return std::ranges::find_if(deviceExtensions,
		[extensionName](const auto& extension)
		{
			return strcmp(extensionName, extension.extensionName) == 0;
		}) != std::ranges::end(deviceExtensions);
}

bool RayQueriesEnabled()
{
	static const bool kEnabled = []
	{
		const char* rayQuery = std::getenv("SPEEDO_RAY_QUERY");
#if defined(__APPLE__)
		// opt in (SPEEDO_RAY_QUERY=1), the user's call (2026-11-08): Metal's ray tracing, which KosmicKrisp's ray queries
		// are, makes the gpu restart beside other gpu work (Apple's bug, reported 2026-11-08), which the mesa port's
		// workaround makes rare rather than impossible (the packaged app lost its device 55 s into a session, its window
		// in front and the browser making previews), and the BVHs perform about the same on the M4. look again once
		// `rtrepro frame 60 rtpudxb` (src/tools/macos) no longer restarts the gpu (docs/notes/indirect-lighting.md)
		return rayQuery != nullptr && std::string_view(rayQuery) == "1";
#else
		return rayQuery == nullptr || std::string_view(rayQuery) != "0";
#endif
	}();
	return kEnabled;
}

bool WantsRayQueries(VkPhysicalDevice device)
{
	return RayQueriesEnabled() && SupportsExtension(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, device) &&
		   SupportsExtension(VK_KHR_RAY_QUERY_EXTENSION_NAME, device) && SupportsExtension(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME, device);
}

bool SupportsExtension(const char* extensionName, VkInstance instance)
{
	static bool gInstanceExtensionsInitialized = false;
	static core::UnorderedMap<VkInstance, std::vector<VkExtensionProperties>> gInstanceExtensions;
	auto& instanceExtensions = gInstanceExtensions[instance];
	if (!gInstanceExtensionsInitialized)
	{
		gInstanceExtensionsInitialized = true;
		
		uint32_t instanceExtensionCount = 0;
		vkEnumerateInstanceExtensionProperties(nullptr, &instanceExtensionCount, nullptr);

		instanceExtensions.resize(instanceExtensionCount);
		vkEnumerateInstanceExtensionProperties(
			nullptr, &instanceExtensionCount, instanceExtensions.data());

		std::ranges::sort(
			instanceExtensions,
			[](const VkExtensionProperties& lhs, const VkExtensionProperties& rhs) { return strcmp(lhs.extensionName, rhs.extensionName) < 0; });

		if constexpr (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
		{
			std::println("{} vulkan instance extension(s) found:", instanceExtensions.size());
			std::ranges::for_each(
				instanceExtensions,
				[](const VkExtensionProperties& instanceExtension) {
					std::println("{}", instanceExtension.extensionName);
				});
		}
	}

	return std::ranges::find_if(instanceExtensions,
		[extensionName](const auto& extension)
		{
			return strcmp(extensionName, extension.extensionName) == 0;
		}) != std::ranges::end(instanceExtensions);
}

template<>
bool SupportsFeature<SwapchainMaintenance1Features<kVk>>(
	const SwapchainMaintenance1Features<kVk>& feature)
{
	return static_cast<bool>(feature.swapchainMaintenance1);
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
		if (((typeFilter & (1 << i)) != 0U) &&
			((memProperties.memoryTypes[i].propertyFlags & properties) != 0U))
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
	VkBufferCreateInfo bufferInfo{.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
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
	allocInfo.pUserData = const_cast<void*>(reinterpret_cast<const void*>(debugName));

	VkBuffer outBuffer;
	VmaAllocation outBufferMemory;
	VK_CHECK(vmaCreateBuffer(allocator, &bufferInfo, &allocInfo, &outBuffer, &outBufferMemory, nullptr));

	VmaAllocatorInfo allocatorInfo;
	vmaGetAllocatorInfo(allocator, &allocatorInfo);
	Track(allocatorInfo.device, VK_OBJECT_TYPE_BUFFER, outBuffer, (debugName != nullptr && *debugName != '\0') ? debugName : "Buffer");

	return std::make_tuple(outBuffer, outBufferMemory);
}

void DestroyBuffer(VmaAllocator allocator, VkBuffer buffer, VmaAllocation memory)
{
	Untrack(VK_OBJECT_TYPE_BUFFER, buffer);
	vmaDestroyBuffer(allocator, buffer, memory);
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
	ENSURE(bufferSize > 0);

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
	VK_CHECK(vmaMapMemory(allocator, memoryHandle, &data));
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
			VK_PIPELINE_STAGE_2_HOST_BIT |
			VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT |
			VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
		break;
	case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
		outAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;;
		outStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
		break;
	case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
		outAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		// depth/stencil is accessed (and cleared by loadOp) in both fragment test stages, not only the late one
		outStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
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
	VkFormat /*format*/,
	VkImageLayout oldLayout,
	VkImageLayout newLayout,
	uint32_t mipLevels,
	VkImageAspectFlags aspectFlags)
{
	TransitionImageLayout(commandBuffer, image, oldLayout, newLayout, 0, mipLevels, aspectFlags);
}

void TransitionImageLayout(
	VkCommandBuffer commandBuffer,
	VkImage image,
	VkImageLayout oldLayout,
	VkImageLayout newLayout,
	uint32_t baseMipLevel,
	uint32_t levelCount,
	VkImageAspectFlags aspectFlags)
{
	VkImageMemoryBarrier2KHR imageBarrier{.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};

	SetDefaultAccessAndStageMasks(oldLayout, imageBarrier.srcAccessMask, imageBarrier.srcStageMask);
	SetDefaultAccessAndStageMasks(newLayout, imageBarrier.dstAccessMask, imageBarrier.dstStageMask);

	// an image from the presentation engine (or a freshly created swapchain) is only available once the acquire
	// semaphore wait is done: the transition must chain after that wait, which a NONE source stage does not
	if (oldLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR || oldLayout == VK_IMAGE_LAYOUT_UNDEFINED)
		imageBarrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

	imageBarrier.oldLayout = oldLayout;
	imageBarrier.newLayout = newLayout;
	imageBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	imageBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	imageBarrier.image = image;
	imageBarrier.subresourceRange.aspectMask = aspectFlags;
	imageBarrier.subresourceRange.baseMipLevel = baseMipLevel;
	imageBarrier.subresourceRange.levelCount = levelCount;
	imageBarrier.subresourceRange.baseArrayLayer = 0UL;
	imageBarrier.subresourceRange.layerCount = 1;

	VkDependencyInfoKHR dependencyInfo{.sType=VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
	dependencyInfo.imageMemoryBarrierCount = 1;
	dependencyInfo.pImageMemoryBarriers = &imageBarrier;

	vkCmdPipelineBarrier2(commandBuffer, &dependencyInfo);
}

std::tuple<VkImage, VmaAllocation> CreateImage(
	VmaAllocator allocator, const VkImageCreateInfo& info, VkMemoryPropertyFlags memoryFlags, const char* debugName)
{
	VmaAllocationCreateInfo allocInfo{};
	allocInfo.flags = VMA_ALLOCATION_CREATE_USER_DATA_COPY_STRING_BIT;
	allocInfo.usage = ((memoryFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0u)
						  ? VMA_MEMORY_USAGE_GPU_ONLY
						  : VMA_MEMORY_USAGE_UNKNOWN;
	allocInfo.requiredFlags = memoryFlags;
	allocInfo.memoryTypeBits = 0UL; // memRequirements.memoryTypeBits;
	allocInfo.pUserData = const_cast<void*>(reinterpret_cast<const void*>(debugName));

	VkImage outImage;
	VmaAllocation outImageMemory;
	VmaAllocationInfo outAllocInfo;
	VK_CHECK(vmaCreateImage(allocator, &info, &allocInfo, &outImage, &outImageMemory, &outAllocInfo));

	VmaAllocatorInfo allocatorInfo;
	vmaGetAllocatorInfo(allocator, &allocatorInfo);
	Track(allocatorInfo.device, VK_OBJECT_TYPE_IMAGE, outImage, (debugName != nullptr && *debugName != '\0') ? debugName : "Image");

	return std::make_tuple(outImage, outImageMemory);
}

VkBuffer CreateAliasingBuffer(
	VmaAllocator allocator, VmaAllocation allocation, VkDeviceSize offset, VkDeviceSize size, VkBufferUsageFlags usage, const char* debugName)
{
	VkBufferCreateInfo bufferInfo{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bufferInfo.size = size;
	bufferInfo.usage = usage;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VkBuffer outBuffer;
	VK_CHECK(vmaCreateAliasingBuffer2(allocator, allocation, offset, &bufferInfo, &outBuffer));

	VmaAllocatorInfo allocatorInfo;
	vmaGetAllocatorInfo(allocator, &allocatorInfo);
	Track(allocatorInfo.device, VK_OBJECT_TYPE_BUFFER, outBuffer, (debugName != nullptr && *debugName != '\0') ? debugName : "Buffer");

	return outBuffer;
}

VkMemoryRequirements GetBufferMemoryRequirements(VmaAllocator allocator, VkDeviceSize size, VkBufferUsageFlags usage)
{
	VmaAllocatorInfo allocatorInfo;
	vmaGetAllocatorInfo(allocator, &allocatorInfo);

	VkBufferCreateInfo bufferInfo{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bufferInfo.size = size;
	bufferInfo.usage = usage;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	// never tracked: it only lives for the query
	VkBuffer buffer;
	VK_CHECK(vkCreateBuffer(allocatorInfo.device, &bufferInfo, nullptr, &buffer));
	VkMemoryRequirements requirements;
	vkGetBufferMemoryRequirements(allocatorInfo.device, buffer, &requirements);
	vkDestroyBuffer(allocatorInfo.device, buffer, nullptr);

	return requirements;
}

VkImageCreateInfo ImageCreateInfo(
	VkExtent3D extent, uint32_t mipLevels, VkFormat format, VkImageTiling tiling, VkImageUsageFlags usage, VkImageLayout initialLayout)
{
	VkImageCreateInfo imageInfo{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	imageInfo.imageType = extent.depth > 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
	imageInfo.extent = extent;
	imageInfo.mipLevels = mipLevels;
	imageInfo.arrayLayers = 1;
	imageInfo.format = format;
	imageInfo.tiling = tiling;
	imageInfo.usage = usage;
	imageInfo.initialLayout = initialLayout;
	imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	return imageInfo;
}

VkImage CreateAliasingImage(
	VmaAllocator allocator, VmaAllocation allocation, VkDeviceSize offset, const VkImageCreateInfo& info, const char* debugName)
{
	VkImage outImage;
	VK_CHECK(vmaCreateAliasingImage2(allocator, allocation, offset, &info, &outImage));

	VmaAllocatorInfo allocatorInfo;
	vmaGetAllocatorInfo(allocator, &allocatorInfo);
	Track(allocatorInfo.device, VK_OBJECT_TYPE_IMAGE, outImage, (debugName != nullptr && *debugName != '\0') ? debugName : "Image");

	return outImage;
}

VkMemoryRequirements GetImageMemoryRequirements(VmaAllocator allocator, const VkImageCreateInfo& info)
{
	VmaAllocatorInfo allocatorInfo;
	vmaGetAllocatorInfo(allocator, &allocatorInfo);

	// never tracked: it only lives for the query
	VkImage image;
	VK_CHECK(vkCreateImage(allocatorInfo.device, &info, nullptr, &image));
	VkMemoryRequirements requirements;
	vkGetImageMemoryRequirements(allocatorInfo.device, image, &requirements);
	vkDestroyImage(allocatorInfo.device, image, nullptr);

	return requirements;
}

void DestroyImage(VmaAllocator allocator, VkImage image, VmaAllocation memory)
{
	Untrack(VK_OBJECT_TYPE_IMAGE, image);
	vmaDestroyImage(allocator, image, memory);
}

VkImageView CreateImageView(
	VkDevice device,
	const VkAllocationCallbacks* hostAllocationCallbacks,
	VkImageViewCreateFlags flags,
	VkImage image,
	VkFormat format,
	VkImageAspectFlags aspectFlags,
	uint32_t mipLevels,
	std::string_view debugName,
	VkComponentMapping components,
	uint32_t baseMipLevel,
	VkImageViewType viewType)
{
	VkImageViewCreateInfo viewInfo{.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	viewInfo.flags = flags;
	viewInfo.image = image;
	viewInfo.viewType = viewType;
	viewInfo.format = format;
	viewInfo.subresourceRange.aspectMask = aspectFlags;
	viewInfo.subresourceRange.baseMipLevel = baseMipLevel;
	viewInfo.subresourceRange.levelCount = mipLevels;
	viewInfo.subresourceRange.baseArrayLayer = 0UL;
	viewInfo.subresourceRange.layerCount = 1;
	viewInfo.components = components; // identity by default (VK_COMPONENT_SWIZZLE_IDENTITY is 0)

	VkImageView outImageView;
	VK_CHECK(vkCreateImageView(device, &viewInfo, hostAllocationCallbacks, &outImageView));

	Track(device, VK_OBJECT_TYPE_IMAGE_VIEW, outImageView, debugName.empty() ? "ImageView" : debugName);

	return outImageView;
}

void DestroyImageView(VkDevice device, const VkAllocationCallbacks* hostAllocationCallbacks, VkImageView imageView)
{
	Untrack(VK_OBJECT_TYPE_IMAGE_VIEW, imageView);
	vkDestroyImageView(device, imageView, hostAllocationCallbacks);
}

VkFramebuffer CreateFramebuffer(
	VkDevice device,
	const VkAllocationCallbacks* hostAllocator,
	VkRenderPass renderPass,
	uint32_t attachmentCount,
	const VkImageView* attachments,
	uint32_t width,
	uint32_t height,
	uint32_t layers,
	std::string_view debugName)
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

	Track(device, VK_OBJECT_TYPE_FRAMEBUFFER, outFramebuffer, debugName.empty() ? "Framebuffer" : debugName);

	return outFramebuffer;
}

void DestroyFramebuffer(VkDevice device, const VkAllocationCallbacks* hostAllocator, VkFramebuffer framebuffer)
{
	Untrack(VK_OBJECT_TYPE_FRAMEBUFFER, framebuffer);
	vkDestroyFramebuffer(device, framebuffer, hostAllocator);
}

VkRenderPass CreateRenderPass(
	VkDevice device,
	const VkAllocationCallbacks* hostAllocator,
	std::span<const VkAttachmentDescription2> attachments,
	std::span<const VkSubpassDescription2> subpasses,
	std::span<const VkSubpassDependency2> subpassDependencies,
	std::string_view debugName)
{
	VkRenderPassCreateInfo2 renderInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2};
	renderInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
	renderInfo.pAttachments = attachments.data();
	renderInfo.subpassCount = static_cast<uint32_t>(subpasses.size());
	renderInfo.pSubpasses = subpasses.data();
	renderInfo.dependencyCount = static_cast<uint32_t>(subpassDependencies.size());
	renderInfo.pDependencies = subpassDependencies.data();

	VkRenderPass outRenderPass;
	VK_CHECK(vkCreateRenderPass2(device, &renderInfo, hostAllocator, &outRenderPass));

	Track(device, VK_OBJECT_TYPE_RENDER_PASS, outRenderPass, debugName.empty() ? "RenderPass" : debugName);

	return outRenderPass;
}

void DestroyRenderPass(VkDevice device, const VkAllocationCallbacks* hostAllocator, VkRenderPass renderPass)
{
	Untrack(VK_OBJECT_TYPE_RENDER_PASS, renderPass);
	vkDestroyRenderPass(device, renderPass, hostAllocator);
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

	VkAttachmentReference2 colorAttachmentRef{.sType=VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2};
	colorAttachmentRef.attachment = 0UL;
	colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	colorAttachmentRef.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;

	VkSubpassDescription2 subpass{.sType=VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2};
	subpass.pipelineBindPoint = bindPoint;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &colorAttachmentRef;

	VkAttachmentReference2 depthAttachmentRef{.sType=VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2};
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

	VkSubpassDependency2 dependency{.sType=VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2};
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
		std::array{subpass},
		std::array{dependency});
}

// the platform's (its window system's surface for the window, see PlatformCreateVulkanSurface)
VkSurfaceKHR CreateSurface(VkInstance instance, const VkAllocationCallbacks* hostAllocator, WindowHandle handle)
{
	VkSurfaceKHR surface = VK_NULL_HANDLE;
	VK_CHECK(static_cast<VkResult>(PlatformCreateVulkanSurface(handle, instance, hostAllocator, &surface)));

	TrackInstance(VK_OBJECT_TYPE_SURFACE_KHR, surface, "Window Surface"); // destroyed by the swapchain that takes it

	return surface;
}
