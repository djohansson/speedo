#pragma once

#include <core/utils.h>
#include <rhi/types.h>

#include <span>

#if (SPEEDO_PROFILING_LEVEL > 0)
void OnCheckFailedDefault(VkResult result, uintptr_t count, ...);
#define VK_CHECK_RESULT_IMPL(Expression, ExpectedResult, FailCallback, ...) do \
{ \
	VkResult __result = (Expression); \
	if (__result != ExpectedResult) \
		FailCallback(__result, SIZEOF__VA_ARGS__(__VA_ARGS__) __VA_OPT__(,) __VA_ARGS__); \
} while(false)
#define VK_CHECK_RESULT(Expression, ExpectedResult, ...) \
	VK_CHECK_RESULT_IMPL(Expression, ExpectedResult, OnCheckFailedDefault __VA_OPT__(,) __VA_ARGS__)
#define VK_CHECK(Expression, ...) \
	VK_CHECK_RESULT_IMPL(Expression, VK_SUCCESS, OnCheckFailedDefault __VA_OPT__(,) __VA_ARGS__)
#else
#define VK_CHECK_RESULT_IMPL(Expression, ExpectedResult, FailCallback, ...) static_cast<void>(Expression)
#define VK_CHECK_RESULT(Expression, ExpectedResult, ...) VK_CHECK_RESULT_IMPL(Expression, ExpectedResult, 0)
#define VK_CHECK(Expression, ...) VK_CHECK_RESULT_IMPL(Expression, VK_SUCCESS, OnCheckFailedDefault)
#endif

extern PFN_vkGetPhysicalDeviceFeatures2 gVkGetPhysicalDeviceFeatures2;
extern PFN_vkGetPhysicalDeviceProperties2 gVkGetPhysicalDeviceProperties2;
extern PFN_vkWaitForPresentKHR gVkWaitForPresentKHR;
extern PFN_vkGetBufferMemoryRequirements2KHR gVkGetBufferMemoryRequirements2KHR;
extern PFN_vkGetImageMemoryRequirements2KHR gVkGetImageMemoryRequirements2KHR;
extern PFN_vkCmdBeginRenderingKHR gVkCmdBeginRenderingKHR;
extern PFN_vkCmdEndRenderingKHR gVkCmdEndRenderingKHR;
#if (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
extern PFN_vkCreateDebugUtilsMessengerEXT gVkCreateDebugUtilsMessengerEXT;
extern PFN_vkDestroyDebugUtilsMessengerEXT gVkDestroyDebugUtilsMessengerEXT;
extern PFN_vkSetDebugUtilsObjectNameEXT gVkSetDebugUtilsObjectNameExt;
#endif
extern PFN_vkCmdSetCheckpointNV gVkCmdSetCheckpointNV;
extern PFN_vkGetQueueCheckpointData2NV gVkGetQueueCheckpointData2NV;
extern PFN_vkCmdPipelineBarrier2KHR gVkCmdPipelineBarrier2KHR;
extern PFN_vkCmdPushDescriptorSetWithTemplateKHR gVkCmdPushDescriptorSetWithTemplateKHR;

void InitInstanceExtensions(VkInstance instance);
void InitDeviceExtensions(VkDevice device);

[[nodiscard]] bool SupportsExtension(const char* extensionName, VkInstance device);
[[nodiscard]] bool SupportsExtension(const char* extensionName, VkPhysicalDevice device);

template<typename T>
[[nodiscard]] bool SupportsFeature(const T& feature);

[[nodiscard]] uint32_t GetFormatSize(VkFormat format, uint32_t& outDivisor);
[[nodiscard]] uint32_t GetFormatSize(VkFormat format);

[[nodiscard]] bool HasColorComponent(VkFormat format);
[[nodiscard]] bool HasStencilComponent(VkFormat format);
[[nodiscard]] bool HasDepthComponent(VkFormat format);

[[nodiscard]] uint32_t
FindMemoryType(VkPhysicalDevice device, uint32_t typeFilter, VkMemoryPropertyFlags properties);

[[nodiscard]] VkFormat
FindSupportedFormat(
	VkPhysicalDevice device,
	std::span<const VkFormat> candidates,
	VkImageTiling tiling,
	VkFormatFeatureFlags features);

void CopyBuffer(
	VkCommandBuffer commandBuffer, VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size);

[[nodiscard]] std::tuple<VkBuffer, VmaAllocation> CreateBuffer(
	VmaAllocator allocator,
	VkDeviceSize size,
	VkBufferUsageFlags usage,
	VkMemoryPropertyFlags flags,
	const char* debugName);

[[nodiscard]] std::tuple<VkBuffer, VmaAllocation> CreateBuffer(
	VkCommandBuffer commandBuffer,
	VmaAllocator allocator,
	VkBuffer stagingBuffer,
	VkDeviceSize bufferSize,
	VkBufferUsageFlags usage,
	VkMemoryPropertyFlags memoryFlags,
	const char* debugName);

[[nodiscard]] std::tuple<VkBuffer, VmaAllocation> CreateStagingBuffer(
	VmaAllocator allocator,
	const void* srcData,
	size_t srcDataSize,
	const char* debugName);

void TransitionImageLayout(
	VkCommandBuffer commandBuffer,
	VkImage image,
	VkFormat format,
	VkImageLayout oldLayout,
	VkImageLayout newLayout,
	uint32_t mipLevels,
	VkImageAspectFlags aspectFlags);

void CopyBufferToImage(
	VkCommandBuffer commandBuffer,
	VkBuffer buffer,
	VkImage image,
	uint32_t width,
	uint32_t height,
	uint32_t mipLevels,
	const uint32_t* mipOffsets,
	uint32_t mipOffsetsStride);

[[nodiscard]] std::tuple<VkImage, VmaAllocation> CreateImage2D(
	VmaAllocator allocator,
	uint32_t width,
	uint32_t height,
	uint32_t mipLevels,
	VkFormat format,
	VkImageTiling tiling,
	VkImageUsageFlags usage,
	VkMemoryPropertyFlags memoryFlags,
	const char* debugName,
	VkImageLayout initialLayout = VK_IMAGE_LAYOUT_UNDEFINED);

[[nodiscard]] std::tuple<VkImage, VmaAllocation> CreateImage2D(
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
	VkImageLayout initialLayout = VK_IMAGE_LAYOUT_UNDEFINED);

[[nodiscard]] VkImageView CreateImageView2D(
	VkDevice device,
	const VkAllocationCallbacks* hostAllocationCallbacks,
	VkImageViewCreateFlags flags,
	VkImage image,
	VkFormat format,
	VkImageAspectFlags aspectFlags,
	uint32_t mipLevels);

[[nodiscard]] VkFramebuffer CreateFramebuffer(
	VkDevice device,
	const VkAllocationCallbacks* hostAllocator,
	VkRenderPass renderPass,
	uint32_t attachmentCount,
	const VkImageView* attachments,
	uint32_t width,
	uint32_t height,
	uint32_t layers);

[[nodiscard]] VkRenderPass CreateRenderPass(
	VkDevice device,
	const VkAllocationCallbacks* hostAllocator,
	std::span<const VkAttachmentDescription2> attachments,
	std::span<const VkSubpassDescription2> subpasses,
	std::span<const VkSubpassDependency2> subpassDependencies);

[[nodiscard]] VkRenderPass CreateRenderPass(
	VkDevice device,
	const VkAllocationCallbacks* hostAllocator,
	VkPipelineBindPoint bindPoint,
	VkFormat colorFormat,
	VkAttachmentLoadOp colorLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
	VkAttachmentStoreOp colorStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
	VkImageLayout colorInitialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	VkImageLayout colorFinalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
	VkFormat depthFormat = VK_FORMAT_UNDEFINED,
	VkAttachmentLoadOp depthLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
	VkAttachmentStoreOp depthStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
	VkImageLayout depthInitialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	VkImageLayout depthFinalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

[[nodiscard]] VkSurfaceKHR CreateSurface(VkInstance instance, const VkAllocationCallbacks* hostAllocator, WindowHandle handle);
