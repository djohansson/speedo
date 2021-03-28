#pragma once
#include "utils.h"

#include <vector>

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#define VK_CHECK(expr) \
    do { \
        VkResult __result = (expr); \
		assertf(__result == VK_SUCCESS, "'%s' line %i failed with %i\n", \
				#expr, __LINE__, __result); \
    } while (0)

static inline void checkResult(VkResult err)
{
	VK_CHECK(err);
}

uint32_t getFormatSize(VkFormat format, uint32_t& outDivisor);
uint32_t getFormatSize(VkFormat format);

bool hasStencilComponent(VkFormat format);
bool hasDepthComponent(VkFormat format);

uint32_t findMemoryType(VkPhysicalDevice device, uint32_t typeFilter,
						VkMemoryPropertyFlags properties);

VkFormat findSupportedFormat(VkPhysicalDevice device, const std::vector<VkFormat>& candidates,
							 VkImageTiling tiling, VkFormatFeatureFlags features);

VkCommandPool createCommandPool(VkDevice device, VkCommandPoolCreateFlags flags,
								int queueFamilyIndex);

std::vector<VkCommandBuffer> allocateCommandBuffers(VkDevice device, VkCommandPool pool,
													VkCommandBufferLevel level, uint32_t count);

VkShaderModule createShaderModule(VkDevice device, size_t codeSize, const uint32_t* codePtr);

VkDescriptorSetLayout createDescriptorSetLayout(
	VkDevice device,
	VkDescriptorSetLayoutCreateFlags flags,
	const VkDescriptorSetLayoutBinding* bindings,
	const VkDescriptorBindingFlags* bindingFlags,
	uint32_t bindingCount);

VkDescriptorUpdateTemplate createDescriptorUpdateTemplate(VkDevice device, const VkDescriptorUpdateTemplateCreateInfo& createInfo);

VkDescriptorSet allocateDescriptorSet(VkDevice device, VkDescriptorPool pool, VkDescriptorSetLayout layout);

std::vector<VkDescriptorSet> allocateDescriptorSets(VkDevice device, VkDescriptorPool pool,
													const VkDescriptorSetLayout* layouts, uint32_t layoutCount);

void copyBuffer(VkCommandBuffer commandBuffer, VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size);

std::tuple<VkBuffer, VmaAllocation> createBuffer(
	VmaAllocator allocator, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags flags,
	const char* debugName);

std::tuple<VkBuffer, VmaAllocation> createBuffer(
	VkCommandBuffer commandBuffer, VmaAllocator allocator, VkBuffer stagingBuffer, VkDeviceSize bufferSize,
	VkBufferUsageFlags usage, VkMemoryPropertyFlags memoryFlags, const char* debugName);

std::tuple<VkBuffer, VmaAllocation> createStagingBuffer(
    VmaAllocator allocator, const void* srcData, size_t srcDataSize, const char* debugName);

VkBufferView createBufferView(VkDevice device, VkBuffer buffer,
	VkBufferViewCreateFlags flags, VkFormat format, VkDeviceSize offset, VkDeviceSize range);

void transitionImageLayout(
	VkCommandBuffer commandBuffer, VkImage image, VkFormat format, VkImageLayout oldLayout,
	VkImageLayout newLayout, uint32_t mipLevels);

void copyBufferToImage(
	VkCommandBuffer commandBuffer, VkBuffer buffer, VkImage image, uint32_t width, uint32_t height, uint32_t mipLevels, const uint32_t* mipOffsets, uint32_t mipOffsetsStride);

std::tuple<VkImage, VmaAllocation> createImage2D(
	VmaAllocator allocator, uint32_t width, uint32_t height, uint32_t mipLevels, VkFormat format, VkImageTiling tiling, 
	VkImageUsageFlags usage, VkMemoryPropertyFlags memoryFlags, const char* debugName, VkImageLayout initialLayout = VK_IMAGE_LAYOUT_UNDEFINED);

std::tuple<VkImage, VmaAllocation> createImage2D(
	VkCommandBuffer commandBuffer, VmaAllocator allocator, VkBuffer stagingBuffer, 
	uint32_t width, uint32_t height, uint32_t mipLevels, const uint32_t* mipOffsets, uint32_t mipOffsetsStride, VkFormat format, VkImageTiling tiling,
	VkImageUsageFlags usage, VkMemoryPropertyFlags memoryFlags, const char* debugName, VkImageLayout initialLayout = VK_IMAGE_LAYOUT_UNDEFINED);

VkImageView createImageView2D(VkDevice device, VkImageViewCreateFlags flags, VkImage image, VkFormat format,
	VkImageAspectFlags aspectFlags, uint32_t mipLevels);

VkSampler createSampler(VkDevice device, const VkSamplerCreateInfo& createInfo);
std::vector<VkSampler> createSamplers(VkDevice device, const std::vector<VkSamplerCreateInfo>& createInfos);

VkFramebuffer createFramebuffer(
	VkDevice device, VkRenderPass renderPass,
	uint32_t attachmentCount, const VkImageView* attachments,
	uint32_t width, uint32_t height, uint32_t layers);

VkRenderPass createRenderPass(
	VkDevice device,
	const std::vector<VkAttachmentDescription>& attachments,
	const std::vector<VkSubpassDescription>& subpasses,
	const std::vector<VkSubpassDependency>& subpassDependencies);

VkRenderPass createRenderPass(
	VkDevice device,
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

VkPipelineLayout createPipelineLayout(
	VkDevice device,
	const VkDescriptorSetLayout* descriptorSetLayouts,
	uint32_t descriptorSetLayoutCount,
	const VkPushConstantRange* pushConstantRanges,
	uint32_t pushConstantRangeCount);

VkSurfaceKHR createSurface(VkInstance instance, void* view);

VmaAllocator createAllocator(VkInstance instance, VkDevice device, VkPhysicalDevice physicalDevice, VkFlags flags);

VkDescriptorPool createDescriptorPool(VkDevice device);

VkResult checkFlipOrPresentResult(VkResult result);
