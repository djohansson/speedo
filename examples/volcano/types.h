#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <cstdint>
#include <type_traits>

enum /*class*/ GraphicsBackend : uint8_t
{
	Vk
};

//using enum GraphicsBackend;

template <GraphicsBackend B>
using ApplicationInfo = std::conditional_t<B == Vk, VkApplicationInfo, std::nullptr_t>;

template <GraphicsBackend B>
using ObjectInfo = std::conditional_t<B == Vk, VkDebugUtilsObjectNameInfoEXT, std::nullptr_t>;

template <GraphicsBackend B>
using Result = std::conditional_t<B == Vk, VkResult, std::nullptr_t>;

template <GraphicsBackend B>
using Flags = std::conditional_t<B == Vk, VkFlags, std::nullptr_t>;

template <GraphicsBackend B>
using DeviceSize = std::conditional_t<B == Vk, VkDeviceSize, std::nullptr_t>;

template <GraphicsBackend B>
using Extent2d = std::conditional_t<B == Vk, VkExtent2D, std::nullptr_t>;

template <GraphicsBackend B>
using Extent3d = std::conditional_t<B == Vk, VkExtent3D, std::nullptr_t>;

template <GraphicsBackend B>
using Viewport = std::conditional_t<B == Vk, VkViewport, std::nullptr_t>;

template <GraphicsBackend B>
using Rect2D = std::conditional_t<B == Vk, VkRect2D, std::nullptr_t>;

template <GraphicsBackend B>
using ObjectType = std::conditional_t<B == Vk, VkObjectType, std::nullptr_t>;

template <GraphicsBackend B>
using InstanceHandle = std::conditional_t<B == Vk, VkInstance, std::nullptr_t>;

template <GraphicsBackend B>
using BufferHandle = std::conditional_t<B == Vk, VkBuffer, std::nullptr_t>;

template <GraphicsBackend B>
using ImageHandle = std::conditional_t<B == Vk, VkImage, std::nullptr_t>;

template <GraphicsBackend B>
using ImageLayout = std::conditional_t<B == Vk, VkImageLayout, std::nullptr_t>;

template <GraphicsBackend B>
using ImageTiling = std::conditional_t<B == Vk, VkImageTiling, std::nullptr_t>;

template <GraphicsBackend B>
using AllocatorHandle = std::conditional_t<B == Vk, VmaAllocator, std::nullptr_t>;

template <GraphicsBackend B>
using AllocationHandle = std::conditional_t<B == Vk, VmaAllocation, std::nullptr_t>;

template <GraphicsBackend B>
using BufferViewHandle = std::conditional_t<B == Vk, VkBufferView, std::nullptr_t>;

template <GraphicsBackend B>
using ImageViewHandle = std::conditional_t<B == Vk, VkImageView, std::nullptr_t>;

template <GraphicsBackend B>
using SurfaceHandle = std::conditional_t<B == Vk, VkSurfaceKHR, std::nullptr_t>;

template <GraphicsBackend B>
using SurfaceFormat = std::conditional_t<B == Vk, VkSurfaceFormatKHR, std::nullptr_t>;

template <GraphicsBackend B>
using Format = std::conditional_t<B == Vk, VkFormat, std::nullptr_t>;

template <GraphicsBackend B>
using ColorSpace = std::conditional_t<B == Vk, VkColorSpaceKHR, std::nullptr_t>;

template <GraphicsBackend B>
using PresentMode = std::conditional_t<B == Vk, VkPresentModeKHR, std::nullptr_t>;

template <GraphicsBackend B>
using SurfaceCapabilities = std::conditional_t<B == Vk, VkSurfaceCapabilitiesKHR, std::nullptr_t>;

template <GraphicsBackend B>
using QueueFamilyProperties = std::conditional_t<B == Vk, VkQueueFamilyProperties, std::nullptr_t>;

template <GraphicsBackend B>
using SwapchainHandle = std::conditional_t<B == Vk, VkSwapchainKHR, std::nullptr_t>;

template <GraphicsBackend B>
using FramebufferHandle = std::conditional_t<B == Vk, VkFramebuffer, std::nullptr_t>;

template <GraphicsBackend B>
using AttachmentDescription = std::conditional_t<B == Vk, VkAttachmentDescription, std::nullptr_t>;

template <GraphicsBackend B>
using AttachmentReference = std::conditional_t<B == Vk, VkAttachmentReference, std::nullptr_t>;

template <GraphicsBackend B>
using SubpassDescription = std::conditional_t<B == Vk, VkSubpassDescription, std::nullptr_t>;

template <GraphicsBackend B>
using SubpassDependency = std::conditional_t<B == Vk, VkSubpassDependency, std::nullptr_t>;

template <GraphicsBackend B>
using AttachmentLoadOp = std::conditional_t<B == Vk, VkAttachmentLoadOp, std::nullptr_t>;

template <GraphicsBackend B>
using AttachmentStoreOp = std::conditional_t<B == Vk, VkAttachmentStoreOp, std::nullptr_t>;

template <GraphicsBackend B>
using DescriptorSetLayoutBinding = std::conditional_t<B == Vk, VkDescriptorSetLayoutBinding, std::nullptr_t>;

template <GraphicsBackend B>
using DescriptorSetLayoutCreateFlags = std::conditional_t<B == Vk, VkDescriptorSetLayoutCreateFlags, std::nullptr_t>;

template <GraphicsBackend B>
using DeviceHandle = std::conditional_t<B == Vk, VkDevice, std::nullptr_t>;

template <GraphicsBackend B>
using PhysicalDeviceHandle = std::conditional_t<B == Vk, VkPhysicalDevice, std::nullptr_t>;

template <GraphicsBackend B>
using PhysicalDeviceProperties = std::conditional_t<B == Vk, VkPhysicalDeviceProperties2, std::nullptr_t>;

template <GraphicsBackend B>
using PhysicalDevicePropertiesEx = std::conditional_t<B == Vk, VkPhysicalDeviceVulkan12Properties, std::nullptr_t>;

template <GraphicsBackend B>
using PhysicalDeviceFeatures = std::conditional_t<B == Vk, VkPhysicalDeviceFeatures2, std::nullptr_t>;

template <GraphicsBackend B>
using PhysicalDeviceFeaturesEx = std::conditional_t<B == Vk, VkPhysicalDeviceVulkan12Features, std::nullptr_t>;

template <GraphicsBackend B>
using PhysicalDeviceRobustnessFeatures = std::conditional_t<B == Vk, VkPhysicalDeviceRobustness2FeaturesEXT, std::nullptr_t>;

template <GraphicsBackend B>
using QueueHandle = std::conditional_t<B == Vk, VkQueue, std::nullptr_t>;

template <GraphicsBackend B>
using CommandBufferHandle = std::conditional_t<B == Vk, VkCommandBuffer, std::nullptr_t>;

template <GraphicsBackend B>
using ShaderModuleHandle = std::conditional_t<B == Vk, VkShaderModule, std::nullptr_t>;

template <GraphicsBackend B>
using DescriptorUpdateTemplateEntry = std::conditional_t<B == Vk, VkDescriptorUpdateTemplateEntry, std::nullptr_t>;

template <GraphicsBackend B>
using DescriptorUpdateTemplateType = std::conditional_t<B == Vk, VkDescriptorUpdateTemplateType, std::nullptr_t>;

template <GraphicsBackend B>
using DescriptorSetLayoutHandle = std::conditional_t<B == Vk, VkDescriptorSetLayout, std::nullptr_t>;

template <GraphicsBackend B>
using DescriptorUpdateTemplateHandle = std::conditional_t<B == Vk, VkDescriptorUpdateTemplate, std::nullptr_t>;

template <GraphicsBackend B>
using PipelineLayoutHandle = std::conditional_t<B == Vk, VkPipelineLayout, std::nullptr_t>;

template <GraphicsBackend B>
using PipelineBindPoint = std::conditional_t<B == Vk, VkPipelineBindPoint, std::nullptr_t>;

template <GraphicsBackend B>
using DescriptorPoolHandle = std::conditional_t<B == Vk, VkDescriptorPool, std::nullptr_t>;

template <GraphicsBackend B>
using DescriptorSetHandle = std::conditional_t<B == Vk, VkDescriptorSet, std::nullptr_t>;

template <GraphicsBackend B>
using RenderPassHandle = std::conditional_t<B == Vk, VkRenderPass, std::nullptr_t>;

template <GraphicsBackend B>
using SubpassContents = std::conditional_t<B == Vk, VkSubpassContents, std::nullptr_t>;

template <GraphicsBackend B>
using PipelineHandle = std::conditional_t<B == Vk, VkPipeline, std::nullptr_t>;

template <GraphicsBackend B>
using PipelineCacheHandle = std::conditional_t<B == Vk, VkPipelineCache, std::nullptr_t>;

template <GraphicsBackend B>
using PipelineShaderStageCreateInfo = std::conditional_t<B == Vk, VkPipelineShaderStageCreateInfo, std::nullptr_t>;

template <GraphicsBackend B>
using PipelineVertexInputStateCreateInfo = std::conditional_t<B == Vk, VkPipelineVertexInputStateCreateInfo, std::nullptr_t>;

template <GraphicsBackend B>
using PipelineInputAssemblyStateCreateInfo = std::conditional_t<B == Vk, VkPipelineInputAssemblyStateCreateInfo, std::nullptr_t>;

template <GraphicsBackend B>
using PipelineViewportStateCreateInfo = std::conditional_t<B == Vk, VkPipelineViewportStateCreateInfo, std::nullptr_t>;

template <GraphicsBackend B>
using PipelineRasterizationStateCreateInfo = std::conditional_t<B == Vk, VkPipelineRasterizationStateCreateInfo, std::nullptr_t>;

template <GraphicsBackend B>
using PipelineMultisampleStateCreateInfo = std::conditional_t<B == Vk, VkPipelineMultisampleStateCreateInfo, std::nullptr_t>;

template <GraphicsBackend B>
using PipelineDepthStencilStateCreateInfo = std::conditional_t<B == Vk, VkPipelineDepthStencilStateCreateInfo, std::nullptr_t>;

template <GraphicsBackend B>
using PipelineColorBlendAttachmentState = std::conditional_t<B == Vk, VkPipelineColorBlendAttachmentState, std::nullptr_t>;

template <GraphicsBackend B>
using PipelineColorBlendStateCreateInfo = std::conditional_t<B == Vk, VkPipelineColorBlendStateCreateInfo, std::nullptr_t>;

template <GraphicsBackend B>
using PipelineDynamicStateCreateInfo = std::conditional_t<B == Vk, VkPipelineDynamicStateCreateInfo, std::nullptr_t>;

template <GraphicsBackend B>
using ShaderStageFlags = std::conditional_t<B == Vk, VkShaderStageFlags, std::nullptr_t>;

template <GraphicsBackend B>
using ShaderStageFlagBits = std::conditional_t<B == Vk, VkShaderStageFlagBits, std::nullptr_t>;

template <GraphicsBackend B>
using DynamicState = std::conditional_t<B == Vk, VkDynamicState, std::nullptr_t>;

template <GraphicsBackend B>
using SamplerHandle = std::conditional_t<B == Vk, VkSampler, std::nullptr_t>;

template <GraphicsBackend B>
using SamplerCreateInfo = std::conditional_t<B == Vk, VkSamplerCreateInfo, std::nullptr_t>;

template <GraphicsBackend B>
using VertexInputBindingDescription = std::conditional_t<B == Vk, VkVertexInputBindingDescription, std::nullptr_t>;

template <GraphicsBackend B>
using VertexInputRate = std::conditional_t<B == Vk, VkVertexInputRate, std::nullptr_t>;

template <GraphicsBackend B>
using VertexInputAttributeDescription = std::conditional_t<B == Vk, VkVertexInputAttributeDescription, std::nullptr_t>;

template <GraphicsBackend B>
using CommandPoolCreateFlags = std::conditional_t<B == Vk, VkCommandPoolCreateFlags, std::nullptr_t>;

template <GraphicsBackend B>
using CommandPoolHandle = std::conditional_t<B == Vk, VkCommandPool, std::nullptr_t>;

template <GraphicsBackend B>
using CommandBufferHandle = std::conditional_t<B == Vk, VkCommandBuffer, std::nullptr_t>;

template <GraphicsBackend B>
using FenceHandle = std::conditional_t<B == Vk, VkFence, std::nullptr_t>;

template <GraphicsBackend B>
using SemaphoreHandle = std::conditional_t<B == Vk, VkSemaphore, std::nullptr_t>;

template <GraphicsBackend B>
using ClearValue = std::conditional_t<B == Vk, VkClearValue, std::nullptr_t>;

template <GraphicsBackend B>
using ClearColorValue = std::conditional_t<B == Vk, VkClearColorValue, std::nullptr_t>;

template <GraphicsBackend B>
using ClearDepthStencilValue = std::conditional_t<B == Vk, VkClearDepthStencilValue, std::nullptr_t>;

template <GraphicsBackend B>
using ClearAttachment = std::conditional_t<B == Vk, VkClearAttachment, std::nullptr_t>;

template <GraphicsBackend B>
using ImageSubresourceRange = std::conditional_t<B == Vk, VkImageSubresourceRange, std::nullptr_t>;

template <GraphicsBackend B>
using CommandBufferBeginInfo = std::conditional_t<B == Vk, VkCommandBufferBeginInfo, std::nullptr_t>;

template <GraphicsBackend B>
using CommandBufferInheritanceInfo = std::conditional_t<B == Vk, VkCommandBufferInheritanceInfo, std::nullptr_t>;

template <GraphicsBackend B>
using CommandBufferLevel = std::conditional_t<B == Vk, VkCommandBufferLevel, std::nullptr_t>;

template <GraphicsBackend B>
using RenderPassBeginInfo = std::conditional_t<B == Vk, VkRenderPassBeginInfo, std::nullptr_t>;

template <GraphicsBackend B>
using SubmitInfo = std::conditional_t<B == Vk, VkSubmitInfo, std::nullptr_t>;

template <GraphicsBackend B>
using TimelineSemaphoreSubmitInfo = std::conditional_t<B == Vk, VkTimelineSemaphoreSubmitInfo, std::nullptr_t>;

template <GraphicsBackend B>
using ImageBlit = std::conditional_t<B == Vk, VkImageBlit, std::nullptr_t>;

template <GraphicsBackend B>
using Filter = std::conditional_t<B == Vk, VkFilter, std::nullptr_t>;

template <GraphicsBackend B>
using ImageSubresourceLayers = std::conditional_t<B == Vk, VkImageSubresourceLayers, std::nullptr_t>;

template <GraphicsBackend B>
using PresentInfo = std::conditional_t<B == Vk, VkPresentInfoKHR, std::nullptr_t>;

template <GraphicsBackend B>
using DescriptorType = std::conditional_t<B == Vk, VkDescriptorType, std::nullptr_t>;

template <GraphicsBackend B>
using DescriptorBufferInfo = std::conditional_t<B == Vk, VkDescriptorBufferInfo, std::nullptr_t>;

template <GraphicsBackend B>
using DescriptorImageInfo = std::conditional_t<B == Vk, VkDescriptorImageInfo, std::nullptr_t>;

template <GraphicsBackend B>
using AccelerationStructureHandle = std::conditional_t<B == Vk, VkAccelerationStructureKHR, std::nullptr_t>;

template <GraphicsBackend B>
using CopyDescriptorSet = std::conditional_t<B == Vk, VkCopyDescriptorSet, std::nullptr_t>;

template <GraphicsBackend B>
using WriteDescriptorSet = std::conditional_t<B == Vk, VkWriteDescriptorSet, std::nullptr_t>;

template <GraphicsBackend B>
using InlineUniformBlock = std::conditional_t<B == Vk, VkWriteDescriptorSetInlineUniformBlockEXT, std::nullptr_t>;

template <GraphicsBackend B>
using PushConstantRange = std::conditional_t<B == Vk, VkPushConstantRange, std::nullptr_t>;

template <GraphicsBackend B>
using DescriptorBindingFlags = std::conditional_t<B == Vk, VkDescriptorBindingFlags, std::nullptr_t>;
