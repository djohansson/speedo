#pragma once
#include "utils.h"

#include <vector>

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

static inline void CHECK_VK(VkResult err)
{
	(void)err;
	assertf(err == VK_SUCCESS, "Vulkan call failed.");
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

VkDescriptorSetLayout createDescriptorSetLayout(VkDevice device, const VkDescriptorSetLayoutBinding* bindings, uint32_t bindingCount);

std::vector<VkDescriptorSet> allocateDescriptorSets(VkDevice device, VkDescriptorPool pool,
													const VkDescriptorSetLayout* layouts, uint32_t layoutCount);

void copyBuffer(VkCommandBuffer commandBuffer, VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size);

std::tuple<VkBuffer, VmaAllocation> createBuffer(
	VmaAllocator allocator, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags flags,
	const char* debugName);

std::tuple<VkBuffer, VmaAllocation> createBuffer(
	VkCommandBuffer commandBuffer, VmaAllocator allocator, VkBuffer stagingBuffer, VkDeviceSize bufferSize,
	VkBufferUsageFlags usage, VkMemoryPropertyFlags memoryFlags, const char* debugName);

VkBufferView createBufferView(VkDevice device, VkBuffer buffer,
	VkBufferViewCreateFlags flags, VkFormat format, VkDeviceSize offset, VkDeviceSize range);

void transitionImageLayout(
	VkCommandBuffer commandBuffer, VkImage image, VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout);

void copyBufferToImage(
	VkCommandBuffer commandBuffer, VkBuffer buffer, VkImage image, uint32_t width, uint32_t height);

std::tuple<VkImage, VmaAllocation> createImage2D(
	VmaAllocator allocator, uint32_t width, uint32_t height, VkFormat format, VkImageTiling tiling, 
	VkImageUsageFlags usage, VkMemoryPropertyFlags memoryFlags, const char* debugName);

std::tuple<VkImage, VmaAllocation> createImage2D(
	VkCommandBuffer commandBuffer, VmaAllocator allocator, VkBuffer stagingBuffer, 
	uint32_t width, uint32_t height, VkFormat format, VkImageTiling tiling, VkImageLayout layout,
	VkImageUsageFlags usage, VkMemoryPropertyFlags memoryFlags, const char* debugName);

VkImageView createImageView2D(VkDevice device, VkImage image, VkFormat format, VkImageAspectFlags aspectFlags);

VkSampler createTextureSampler(VkDevice device);

VkFramebuffer createFramebuffer(
	VkDevice device, VkRenderPass renderPass,
	uint32_t attachmentCount, const VkImageView* attachments,
	uint32_t width, uint32_t height, uint32_t layers);

VkRenderPass createRenderPass(VkDevice device, VkFormat colorFormat, VkFormat depthFormat);

VkSurfaceKHR createSurface(VkInstance instance, void* view);

VkResult checkFlipOrPresentResult(VkResult result);


static inline bool operator==(VkSurfaceFormatKHR lhs, const VkSurfaceFormatKHR& rhs)
{
	return lhs.format == rhs.format && lhs.colorSpace == rhs.colorSpace;
}
