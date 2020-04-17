#pragma once

#include "utils.h"
#include "vk-utils.h"

#include <cstdint>
#include <type_traits>


enum class GraphicsBackend
{
	Vulkan
};

template <GraphicsBackend B>
using InstanceCreateDesc = std::conditional_t<B == GraphicsBackend::Vulkan, VkApplicationInfo, std::nullptr_t>;

template <GraphicsBackend B>
using Result = std::conditional_t<B == GraphicsBackend::Vulkan, VkResult, std::nullptr_t>;

template <GraphicsBackend B>
using Flags = std::conditional_t<B == GraphicsBackend::Vulkan, VkFlags, std::nullptr_t>;

template <GraphicsBackend B>
using DeviceSize = std::conditional_t<B == GraphicsBackend::Vulkan, VkDeviceSize, std::nullptr_t>;

template <GraphicsBackend B>
using Extent2d = std::conditional_t<B == GraphicsBackend::Vulkan, VkExtent2D, std::nullptr_t>;

template <GraphicsBackend B>
using InstanceHandle = std::conditional_t<B == GraphicsBackend::Vulkan, VkInstance, std::nullptr_t>;

template <GraphicsBackend B>
using BufferHandle = std::conditional_t<B == GraphicsBackend::Vulkan, VkBuffer, std::nullptr_t>;

template <GraphicsBackend B>
using ImageHandle = std::conditional_t<B == GraphicsBackend::Vulkan, VkImage, std::nullptr_t>;

template <GraphicsBackend B>
using AllocatorHandle = std::conditional_t<B == GraphicsBackend::Vulkan, VmaAllocator, std::nullptr_t>;

template <GraphicsBackend B>
using AllocationHandle = std::conditional_t<B == GraphicsBackend::Vulkan, VmaAllocation, std::nullptr_t>;

template <GraphicsBackend B>
using BufferViewHandle = std::conditional_t<B == GraphicsBackend::Vulkan, VkBufferView, std::nullptr_t>;

template <GraphicsBackend B>
using ImageViewHandle = std::conditional_t<B == GraphicsBackend::Vulkan, VkImageView, std::nullptr_t>;

template <GraphicsBackend B>
using SurfaceHandle = std::conditional_t<B == GraphicsBackend::Vulkan, VkSurfaceKHR, std::nullptr_t>;

template <GraphicsBackend B>
using SurfaceFormat =
	std::conditional_t<B == GraphicsBackend::Vulkan, VkSurfaceFormatKHR, std::nullptr_t>;

template <GraphicsBackend B>
using Format = std::conditional_t<B == GraphicsBackend::Vulkan, VkFormat, std::nullptr_t>;

template <GraphicsBackend B>
using ColorSpace = std::conditional_t<B == GraphicsBackend::Vulkan, VkColorSpaceKHR, std::nullptr_t>;

template <GraphicsBackend B>
using PresentMode =
	std::conditional_t<B == GraphicsBackend::Vulkan, VkPresentModeKHR, std::nullptr_t>;

template <GraphicsBackend B>
using SurfaceCapabilities =
	std::conditional_t<B == GraphicsBackend::Vulkan, VkSurfaceCapabilitiesKHR, std::nullptr_t>;

template <GraphicsBackend B>
using PresentMode =
	std::conditional_t<B == GraphicsBackend::Vulkan, VkPresentModeKHR, std::nullptr_t>;

template <GraphicsBackend B>
using SwapchainHandle = std::conditional_t<B == GraphicsBackend::Vulkan, VkSwapchainKHR, std::nullptr_t>;

template <GraphicsBackend B>
using FramebufferHandle = std::conditional_t<B == GraphicsBackend::Vulkan, VkFramebuffer, std::nullptr_t>;

template <GraphicsBackend B>
using DescriptorSetLayoutBinding =
	std::conditional_t<B == GraphicsBackend::Vulkan, VkDescriptorSetLayoutBinding, std::nullptr_t>;

template <GraphicsBackend B>
using DeviceHandle = std::conditional_t<B == GraphicsBackend::Vulkan, VkDevice, std::nullptr_t>;

template <GraphicsBackend B>
using PhysicalDeviceHandle =
	std::conditional_t<B == GraphicsBackend::Vulkan, VkPhysicalDevice, std::nullptr_t>;

template <GraphicsBackend B>
using PhysicalDeviceProperties =
	std::conditional_t<B == GraphicsBackend::Vulkan, VkPhysicalDeviceProperties, std::nullptr_t>;

template <GraphicsBackend B>
using QueueHandle = std::conditional_t<B == GraphicsBackend::Vulkan, VkQueue, std::nullptr_t>;

template <GraphicsBackend B>
using CommandBufferHandle = std::conditional_t<B == GraphicsBackend::Vulkan, VkCommandBuffer, std::nullptr_t>;

template <GraphicsBackend B>
using ShaderModuleHandle =
	std::conditional_t<B == GraphicsBackend::Vulkan, VkShaderModule, std::nullptr_t>;

template <GraphicsBackend B>
using DescriptorSetLayoutHandle =
	std::conditional_t<B == GraphicsBackend::Vulkan, VkDescriptorSetLayout, std::nullptr_t>;

template <GraphicsBackend B>
using PipelineLayoutHandle =
	std::conditional_t<B == GraphicsBackend::Vulkan, VkPipelineLayout, std::nullptr_t>;

template <GraphicsBackend B>
using DescriptorPoolHandle = std::conditional_t<B == GraphicsBackend::Vulkan, VkDescriptorPool, std::nullptr_t>;

template <GraphicsBackend B>
using DescriptorSetHandle =
	std::conditional_t<B == GraphicsBackend::Vulkan, VkDescriptorSet, std::nullptr_t>;

template <GraphicsBackend B>
using RenderPassHandle = std::conditional_t<B == GraphicsBackend::Vulkan, VkRenderPass, std::nullptr_t>;

template <GraphicsBackend B>
using PipelineHandle = std::conditional_t<B == GraphicsBackend::Vulkan, VkPipeline, std::nullptr_t>;

template <GraphicsBackend B>
using PipelineCacheHandle = std::conditional_t<B == GraphicsBackend::Vulkan, VkPipelineCache, std::nullptr_t>;

template <GraphicsBackend B>
using SamplerHandle = std::conditional_t<B == GraphicsBackend::Vulkan, VkSampler, std::nullptr_t>;

template <GraphicsBackend B>
using VertexInputBindingDescription = std::conditional_t<
	B == GraphicsBackend::Vulkan, VkVertexInputBindingDescription, std::nullptr_t>;

template <GraphicsBackend B>
using VertexInputRate =
	std::conditional_t<B == GraphicsBackend::Vulkan, VkVertexInputRate, std::nullptr_t>;

template <GraphicsBackend B>
using VertexInputAttributeDescription = std::conditional_t<
	B == GraphicsBackend::Vulkan, VkVertexInputAttributeDescription, std::nullptr_t>;

template <GraphicsBackend B>
using CommandPoolHandle = std::conditional_t<B == GraphicsBackend::Vulkan, VkCommandPool, std::nullptr_t>;

template <GraphicsBackend B>
using CommandBufferHandle = std::conditional_t<B == GraphicsBackend::Vulkan, VkCommandBuffer, std::nullptr_t>;

template <GraphicsBackend B>
using FenceHandle = std::conditional_t<B == GraphicsBackend::Vulkan, VkFence, std::nullptr_t>;

template <GraphicsBackend B>
using SemaphoreHandle = std::conditional_t<B == GraphicsBackend::Vulkan, VkSemaphore, std::nullptr_t>;

template <GraphicsBackend B>
using ClearValue = std::conditional_t<B == GraphicsBackend::Vulkan, VkClearValue, std::nullptr_t>;

template <GraphicsBackend B>
using CommandBufferBeginInfo = std::conditional_t<B == GraphicsBackend::Vulkan, VkCommandBufferBeginInfo, std::nullptr_t>;

template <GraphicsBackend B>
using CommandBufferInheritanceInfo = std::conditional_t<B == GraphicsBackend::Vulkan, VkCommandBufferInheritanceInfo, std::nullptr_t>;

template <GraphicsBackend B>
using RenderPassBeginInfo = std::conditional_t<B == GraphicsBackend::Vulkan, VkRenderPassBeginInfo, std::nullptr_t>;
