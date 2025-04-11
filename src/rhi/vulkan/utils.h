#pragma once

#include <vulkan/vulkan.h>
#include <vulkan/vk_enum_string_helper.h>

#if defined(__WINDOWS__)
#include <vma/vk_mem_alloc.h>
#else
#include <vk_mem_alloc.h>
#endif

#include <core/assert.h>
#include <core/utils.h>
#include <rhi/types.h>

#include <span>

#if (SPEEDO_PROFILING_LEVEL > 0)
#ifdef __cplusplus
#	define VK_ENSURE(A) ENSUREF((A == VK_SUCCESS), "{} failed with return code {}", #A, string_VkResult(A))
#	define VK_ENSURE_RESULT(A, C) ENSUREF((A == C), "{} failed with return code {}", #A, string_VkResult(A))
#	define VK_ASSERT(A) ASSERTF((A == VK_SUCCESS), "{} failed with return code {}", #A, string_VkResult(A))
#	define VK_ASSERT_OTHER(A, C) ASSERTF((A == C), "{} failed with return code {}", #A, string_VkResult(A))
#else
#	define VK_ENSURE(A) ENSUREF((A == VK_SUCCESS), "%s failed with return code %s", #A, string_VkResult(A))
#	define VK_ENSURE_RESULT(A, C) ENSUREF((A == C), "%s failed with return code %s", #A, string_VkResult(A))
#	define VK_ASSERT(A) ASSERTF((A == VK_SUCCESS), "%s failed with return code %s", #A, string_VkResult(A))
#	define VK_ASSERT_OTHER(A, C) ASSERTF((A == C), "%s failed with return code %s", #A, string_VkResult(A))
#endif
#else
#	define VK_ENSURE(A) static_cast<void>(A);
#	define VK_ENSURE_RESULT(A, C) static_cast<void>(A);
#	define VK_ASSERT(A) static_cast<void>(A);
#	define VK_ASSERT_OTHER(A, C) static_cast<void>(A);
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
extern PFN_vkCmdPipelineBarrier2KHR gVkCmdPipelineBarrier2KHR;
extern PFN_vkCmdPushDescriptorSetWithTemplateKHR gVkCmdPushDescriptorSetWithTemplateKHR;

void InitInstanceExtensions(VkInstance instance);
void InitDeviceExtensions(VkDevice device);

[[nodiscard]] bool SupportsExtension(const char* extensionName, VkInstance device);
[[nodiscard]] bool SupportsExtension(const char* extensionName, VkPhysicalDevice device);


bool HasColorComponent(VkFormat format);
bool HasStencilComponent(VkFormat format);
bool HasDepthComponent(VkFormat format);

uint32_t
FindMemoryType(VkPhysicalDevice device, uint32_t typeFilter, VkMemoryPropertyFlags properties);

VkFormat FindSupportedFormat(
	VkPhysicalDevice device,
	std::span<const VkFormat> candidates,
	VkImageTiling tiling,
	VkFormatFeatureFlags features);

void CopyBuffer(
	VkCommandBuffer commandBuffer, VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size);

std::tuple<VkBuffer, VmaAllocation> CreateBuffer(
	VmaAllocator allocator,
	VkDeviceSize size,
	VkBufferUsageFlags usage,
	VkMemoryPropertyFlags flags,
	const char* debugName);

std::tuple<VkBuffer, VmaAllocation> CreateBuffer(
	VkCommandBuffer commandBuffer,
	VmaAllocator allocator,
	VkBuffer stagingBuffer,
	VkDeviceSize bufferSize,
	VkBufferUsageFlags usage,
	VkMemoryPropertyFlags memoryFlags,
	const char* debugName);

std::tuple<VkBuffer, VmaAllocation> CreateStagingBuffer(
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
	VkImageLayout initialLayout = VK_IMAGE_LAYOUT_UNDEFINED);

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
	VkImageLayout initialLayout = VK_IMAGE_LAYOUT_UNDEFINED);

VkImageView CreateImageView2D(
	VkDevice device,
	const VkAllocationCallbacks* hostAllocationCallbacks,
	VkImageViewCreateFlags flags,
	VkImage image,
	VkFormat format,
	VkImageAspectFlags aspectFlags,
	uint32_t mipLevels);

VkFramebuffer CreateFramebuffer(
	VkDevice device,
	const VkAllocationCallbacks* hostAllocator,
	VkRenderPass renderPass,
	uint32_t attachmentCount,
	const VkImageView* attachments,
	uint32_t width,
	uint32_t height,
	uint32_t layers);

VkRenderPass CreateRenderPass(
	VkDevice device,
	const VkAllocationCallbacks* hostAllocator,
	std::span<const VkAttachmentDescription2> attachments,
	std::span<const VkSubpassDescription2> subpasses,
	std::span<const VkSubpassDependency2> subpassDependencies);

VkRenderPass CreateRenderPass(
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

VkSurfaceKHR CreateSurface(VkInstance instance, const VkAllocationCallbacks* hostAllocator, void* view);

VkResult CheckFlipOrPresentResult(VkResult result);
