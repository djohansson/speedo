#pragma once

#include <cassert>
#include <cstdint>
#include <optional>
#include <map>
#include <vector>

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

static inline void CHECK_VK(VkResult err)
{
	(void)err;
	assert(err == VK_SUCCESS);
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

VkCommandBuffer beginSingleTimeCommands(VkDevice device, VkCommandPool commandPool);

void endSingleTimeCommands(VkDevice device, VkQueue queue, VkCommandBuffer commandBuffer,
						   VkCommandPool commandPool);

void copyBuffer(VkDevice device, VkCommandPool commandPool, VkQueue queue, VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size);

std::tuple<VkBuffer, VmaAllocation> createBuffer(
	VmaAllocator allocator, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags flags,
	const char* debugName);

std::tuple<VkBuffer, VmaAllocation> createBuffer(
	VkDevice device, VkCommandPool commandPool, VkQueue queue, VmaAllocator allocator,
	VkBuffer stagingBuffer, VkDeviceSize bufferSize, VkBufferUsageFlags usage, VkMemoryPropertyFlags memoryFlags, const char* debugName);

VkBufferView createBufferView(VkDevice device, VkBuffer buffer,
	VkBufferViewCreateFlags flags, VkFormat format, VkDeviceSize offset, VkDeviceSize range);

void transitionImageLayout(
	VkDevice device, VkCommandPool commandPool, VkQueue queue, 
	VkImage image, VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout);

void copyBufferToImage(
	VkDevice device, VkCommandPool commandPool, VkQueue queue, VmaAllocator allocator,
	VkBuffer buffer, VkImage image, uint32_t width, uint32_t height);

std::tuple<VkImage, VmaAllocation> createImage2D(
	VmaAllocator allocator,
	uint32_t width, uint32_t height, VkFormat format, VkImageTiling tiling, VkImageLayout layout,
	VkImageUsageFlags usage, VkMemoryPropertyFlags memoryFlags, const char* debugName);

std::tuple<VkImage, VmaAllocation> createImage2D(
	VkDevice device, VkCommandPool commandPool, VkQueue queue, VmaAllocator allocator,
	VkBuffer stagingBuffer, 
	uint32_t width, uint32_t height, VkFormat format, VkImageTiling tiling, VkImageLayout layout,
	VkImageUsageFlags usage, VkMemoryPropertyFlags memoryFlags, const char* debugName);

VkImageView createImageView2D(VkDevice device, VkImage image, VkFormat format, VkImageAspectFlags aspectFlags);

VkSampler createTextureSampler(VkDevice device);

static inline bool operator==(VkSurfaceFormatKHR lhs, const VkSurfaceFormatKHR& rhs)
{
	return lhs.format == rhs.format && lhs.colorSpace == rhs.colorSpace;
}
