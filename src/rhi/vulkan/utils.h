#pragma once

#include <vulkan/vulkan.h>
#include <vulkan/vk_enum_string_helper.h>

#if defined(__WINDOWS__)
#include <vma/vk_mem_alloc.h>
#else
#include <vk_mem_alloc.h>
#endif

#include <core/utils.h>
#include <rhi/types.h>

#include <vector>

#if PROFILING_ENABLED
#	define VK_CHECK(expr)                                                                         \
		do                                                                                         \
		{                                                                                          \
			VkResult __result = (expr);                                                            \
			assertf(                                                                               \
				__result == VK_SUCCESS,                                                            \
				"'%s' failed with %s",                                                             \
				#expr,                                                                             \
				string_VkResult(__result));                                                        \
		} while (0)
#else
#	define VK_CHECK(expr) static_cast<void>(expr);
#endif

uint32_t getFormatSize(VkFormat format, uint32_t& outDivisor);
uint32_t getFormatSize(VkFormat format);

bool hasStencilComponent(VkFormat format);
bool hasDepthComponent(VkFormat format);

uint32_t
findMemoryType(VkPhysicalDevice device, uint32_t typeFilter, VkMemoryPropertyFlags properties);

VkFormat findSupportedFormat(
	VkPhysicalDevice device,
	const std::vector<VkFormat>& candidates,
	VkImageTiling tiling,
	VkFormatFeatureFlags features);

std::vector<VkCommandBuffer> allocateCommandBuffers(
	VkDevice device,
	VkCommandPool pool,
	VkCommandBufferLevel level,
	uint32_t count);

VkShaderModule createShaderModule(
	VkDevice device,
	const VkAllocationCallbacks* hostAllocator,
	size_t codeSize,
	const uint32_t* codePtr);

VkDescriptorSetLayout createDescriptorSetLayout(
	VkDevice device,
	const VkAllocationCallbacks* hostAllocator,
	VkDescriptorSetLayoutCreateFlags flags,
	const VkDescriptorSetLayoutBinding* bindings,
	const VkDescriptorBindingFlags* bindingFlags,
	uint32_t bindingCount);

VkDescriptorSet
allocateDescriptorSet(VkDevice device, VkDescriptorPool pool, VkDescriptorSetLayout layout);

std::vector<VkDescriptorSet> allocateDescriptorSets(
	VkDevice device,
	VkDescriptorPool pool,
	const VkDescriptorSetLayout* layouts,
	uint32_t layoutCount);

void copyBuffer(
	VkCommandBuffer commandBuffer, VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size);

std::tuple<VkBuffer, VmaAllocation> createBuffer(
	VmaAllocator allocator,
	VkDeviceSize size,
	VkBufferUsageFlags usage,
	VkMemoryPropertyFlags flags,
	const char* debugName);

std::tuple<VkBuffer, VmaAllocation> createBuffer(
	VkCommandBuffer commandBuffer,
	VmaAllocator allocator,
	VkBuffer stagingBuffer,
	VkDeviceSize bufferSize,
	VkBufferUsageFlags usage,
	VkMemoryPropertyFlags memoryFlags,
	const char* debugName);

std::tuple<VkBuffer, VmaAllocation> createStagingBuffer(
	VmaAllocator allocator, const void* srcData, size_t srcDataSize, const char* debugName);

void transitionImageLayout(
	VkCommandBuffer commandBuffer,
	VkImage image,
	VkFormat format,
	VkImageLayout oldLayout,
	VkImageLayout newLayout,
	uint32_t mipLevels);

void copyBufferToImage(
	VkCommandBuffer commandBuffer,
	VkBuffer buffer,
	VkImage image,
	uint32_t width,
	uint32_t height,
	uint32_t mipLevels,
	const uint32_t* mipOffsets,
	uint32_t mipOffsetsStride);

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
	VkImageLayout initialLayout = VK_IMAGE_LAYOUT_UNDEFINED);

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
	VkImageLayout initialLayout = VK_IMAGE_LAYOUT_UNDEFINED);

VkImageView createImageView2D(
	VkDevice device,
	const VkAllocationCallbacks* hostAllocationCallbacks,
	VkImageViewCreateFlags flags,
	VkImage image,
	VkFormat format,
	VkImageAspectFlags aspectFlags,
	uint32_t mipLevels);

VkFramebuffer createFramebuffer(
	VkDevice device,
	const VkAllocationCallbacks* hostAllocator,
	VkRenderPass renderPass,
	uint32_t attachmentCount,
	const VkImageView* attachments,
	uint32_t width,
	uint32_t height,
	uint32_t layers);

VkRenderPass createRenderPass(
	VkDevice device,
	const VkAllocationCallbacks* hostAllocator,
	const std::vector<VkAttachmentDescription>& attachments,
	const std::vector<VkSubpassDescription>& subpasses,
	const std::vector<VkSubpassDependency>& subpassDependencies);

VkRenderPass createRenderPass(
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

VkSurfaceKHR createSurface(VkInstance instance, const VkAllocationCallbacks* hostAllocator, void* view);

VkResult checkFlipOrPresentResult(VkResult result);
