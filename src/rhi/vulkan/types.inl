#include <type_traits>
#include <variant>

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_beta.h>

#if defined(__WINDOWS__)
#include <vma/vk_mem_alloc.h>
#else
#include <vk_mem_alloc.h>
#endif
template <GraphicsApi G>
using AllocationCallbacks = std::conditional_t<G == kVk, VkAllocationCallbacks, std::nullptr_t>;

template <GraphicsApi G>
using ApplicationInfo = std::conditional_t<G == kVk, VkApplicationInfo, std::nullptr_t>;

template <GraphicsApi G>
using SystemAllocationScope  = std::conditional_t<G == kVk, VkSystemAllocationScope, std::nullptr_t>;;

template <GraphicsApi G>
using ObjectInfo = std::conditional_t<G == kVk, VkDebugUtilsObjectNameInfoEXT, std::nullptr_t>;

template <GraphicsApi G>
using Result = std::conditional_t<G == kVk, VkResult, std::nullptr_t>;

template <GraphicsApi G>
using Flags = std::conditional_t<G == kVk, VkFlags, std::nullptr_t>;

template <GraphicsApi G>
using DeviceSize = std::conditional_t<G == kVk, VkDeviceSize, std::nullptr_t>;

template <GraphicsApi G>
using Extent2d = std::conditional_t<G == kVk, VkExtent2D, std::nullptr_t>;

template <GraphicsApi G>
using Extent3d = std::conditional_t<G == kVk, VkExtent3D, std::nullptr_t>;

template <GraphicsApi G>
using Viewport = std::conditional_t<G == kVk, VkViewport, std::nullptr_t>;

template <GraphicsApi G>
using Rect2D = std::conditional_t<G == kVk, VkRect2D, std::nullptr_t>;

template <GraphicsApi G>
using ObjectType = std::conditional_t<G == kVk, VkObjectType, std::nullptr_t>;

template <GraphicsApi G>
using InstanceHandle = std::conditional_t<G == kVk, VkInstance, std::nullptr_t>;

template <GraphicsApi G>
using BufferHandle = std::conditional_t<G == kVk, VkBuffer, std::nullptr_t>;

template <GraphicsApi G>
using ImageHandle = std::conditional_t<G == kVk, VkImage, std::nullptr_t>;

template <GraphicsApi G>
using ImageLayout = std::conditional_t<G == kVk, VkImageLayout, std::nullptr_t>;

template <GraphicsApi G>
using ImageTiling = std::conditional_t<G == kVk, VkImageTiling, std::nullptr_t>;

template <GraphicsApi G>
using AllocatorHandle = std::conditional_t<G == kVk, VmaAllocator, std::nullptr_t>;

template <GraphicsApi G>
using AllocationHandle = std::conditional_t<G == kVk, VmaAllocation, std::nullptr_t>;

template <GraphicsApi G>
using BufferViewHandle = std::conditional_t<G == kVk, VkBufferView, std::nullptr_t>;

template <GraphicsApi G>
using ImageViewHandle = std::conditional_t<G == kVk, VkImageView, std::nullptr_t>;

template <GraphicsApi G>
using SurfaceHandle = std::conditional_t<G == kVk, VkSurfaceKHR, std::nullptr_t>;

template <GraphicsApi G>
using SurfaceFormat = std::conditional_t<G == kVk, VkSurfaceFormatKHR, std::nullptr_t>;

template <GraphicsApi G>
using Format = std::conditional_t<G == kVk, VkFormat, std::nullptr_t>;

template <GraphicsApi G>
using ColorSpace = std::conditional_t<G == kVk, VkColorSpaceKHR, std::nullptr_t>;

template <GraphicsApi G>
using PresentMode = std::conditional_t<G == kVk, VkPresentModeKHR, std::nullptr_t>;

template <GraphicsApi G>
using SurfaceCapabilities = std::conditional_t<G == kVk, VkSurfaceCapabilitiesKHR, std::nullptr_t>;

template <GraphicsApi G>
using QueueFamilyProperties = std::conditional_t<G == kVk, VkQueueFamilyProperties, std::nullptr_t>;

template <GraphicsApi G>
using SwapchainHandle = std::conditional_t<G == kVk, VkSwapchainKHR, std::nullptr_t>;

template <GraphicsApi G>
using FramebufferHandle = std::conditional_t<G == kVk, VkFramebuffer, std::nullptr_t>;

template <GraphicsApi G>
using AttachmentDescription = std::conditional_t<G == kVk, VkAttachmentDescription2, std::nullptr_t>;

template <GraphicsApi G>
using AttachmentReference = std::conditional_t<G == kVk, VkAttachmentReference2, std::nullptr_t>;

template <GraphicsApi G>
using SubpassDescription = std::conditional_t<G == kVk, VkSubpassDescription2 , std::nullptr_t>;

template <GraphicsApi G>
using SubpassDependency = std::conditional_t<G == kVk, VkSubpassDependency2, std::nullptr_t>;

template <GraphicsApi G>
using AttachmentLoadOp = std::conditional_t<G == kVk, VkAttachmentLoadOp, std::nullptr_t>;

template <GraphicsApi G>
using AttachmentStoreOp = std::conditional_t<G == kVk, VkAttachmentStoreOp, std::nullptr_t>;

template <GraphicsApi G>
using DescriptorSetLayoutBinding =
	std::conditional_t<G == kVk, VkDescriptorSetLayoutBinding, std::nullptr_t>;

template <GraphicsApi G>
using DescriptorSetLayoutCreateFlags =
	std::conditional_t<G == kVk, VkDescriptorSetLayoutCreateFlags, std::nullptr_t>;

template <GraphicsApi G>
using DeviceHandle = std::conditional_t<G == kVk, VkDevice, std::nullptr_t>;

template <GraphicsApi G>
using PhysicalDeviceHandle = std::conditional_t<G == kVk, VkPhysicalDevice, std::nullptr_t>;

template <GraphicsApi G>
using PhysicalDeviceProperties =
	std::conditional_t<G == kVk, VkPhysicalDeviceProperties2, std::nullptr_t>;

template <GraphicsApi G>
using PhysicalDeviceProperties12Ex =
	std::conditional_t<G == kVk, VkPhysicalDeviceVulkan12Properties, std::nullptr_t>;

template <GraphicsApi G>
using PhysicalDeviceProperties13Ex =
	std::conditional_t<G == kVk, VkPhysicalDeviceVulkan13Properties, std::nullptr_t>;

template <GraphicsApi G>
using PhysicalDeviceFeatures =
	std::conditional_t<G == kVk, VkPhysicalDeviceFeatures2, std::nullptr_t>;

template <GraphicsApi G>
using PhysicalDeviceFeatures12Ex =
	std::conditional_t<G == kVk, VkPhysicalDeviceVulkan12Features, std::nullptr_t>;

template <GraphicsApi G>
using PhysicalDeviceFeatures13Ex =
	std::conditional_t<G == kVk, VkPhysicalDeviceVulkan13Features, std::nullptr_t>;

template <GraphicsApi G>
using PhysicalDeviceInlineUniformBlockFeatures =
	std::conditional_t<G == kVk, VkPhysicalDeviceInlineUniformBlockFeatures, std::nullptr_t>;

template <GraphicsApi G>
using PhysicalDeviceInlineUniformBlockProperties =
	std::conditional_t<G == kVk, VkPhysicalDeviceInlineUniformBlockProperties, std::nullptr_t>;

template <GraphicsApi G>
using QueueHandle = std::conditional_t<G == kVk, VkQueue, std::nullptr_t>;

template <GraphicsApi G>
using CommandBufferHandle = std::conditional_t<G == kVk, VkCommandBuffer, std::nullptr_t>;

template <GraphicsApi G>
using ShaderModuleHandle = std::conditional_t<G == kVk, VkShaderModule, std::nullptr_t>;

template <GraphicsApi G>
using DescriptorUpdateTemplateEntry =
	std::conditional_t<G == kVk, VkDescriptorUpdateTemplateEntry, std::nullptr_t>;

template <GraphicsApi G>
using DescriptorUpdateTemplateType =
	std::conditional_t<G == kVk, VkDescriptorUpdateTemplateType, std::nullptr_t>;

template <GraphicsApi G>
using DescriptorSetLayoutHandle =
	std::conditional_t<G == kVk, VkDescriptorSetLayout, std::nullptr_t>;

template <GraphicsApi G>
using DescriptorUpdateTemplateHandle =
	std::conditional_t<G == kVk, VkDescriptorUpdateTemplate, std::nullptr_t>;

template <GraphicsApi G>
using PipelineLayoutHandle = std::conditional_t<G == kVk, VkPipelineLayout, std::nullptr_t>;

template <GraphicsApi G>
using PipelineBindPoint = std::conditional_t<G == kVk, VkPipelineBindPoint, std::nullptr_t>;

template <GraphicsApi G>
using DescriptorPoolHandle = std::conditional_t<G == kVk, VkDescriptorPool, std::nullptr_t>;

template <GraphicsApi G>
using DescriptorSetHandle = std::conditional_t<G == kVk, VkDescriptorSet, std::nullptr_t>;

template <GraphicsApi G>
using RenderPassHandle = std::conditional_t<G == kVk, VkRenderPass, std::nullptr_t>;

template <GraphicsApi G>
using SubpassContents = std::conditional_t<G == kVk, VkSubpassContents, std::nullptr_t>;

template <GraphicsApi G>
using PipelineHandle = std::conditional_t<G == kVk, VkPipeline, std::nullptr_t>;

template <GraphicsApi G>
using PipelineCacheHandle = std::conditional_t<G == kVk, VkPipelineCache, std::nullptr_t>;

template <GraphicsApi G>
using PipelineShaderStageCreateInfo =
	std::conditional_t<G == kVk, VkPipelineShaderStageCreateInfo, std::nullptr_t>;

template <GraphicsApi G>
using PipelineVertexInputStateCreateInfo =
	std::conditional_t<G == kVk, VkPipelineVertexInputStateCreateInfo, std::nullptr_t>;

template <GraphicsApi G>
using PipelineInputAssemblyStateCreateInfo =
	std::conditional_t<G == kVk, VkPipelineInputAssemblyStateCreateInfo, std::nullptr_t>;

template <GraphicsApi G>
using PipelineViewportStateCreateInfo =
	std::conditional_t<G == kVk, VkPipelineViewportStateCreateInfo, std::nullptr_t>;

template <GraphicsApi G>
using PipelineRasterizationStateCreateInfo =
	std::conditional_t<G == kVk, VkPipelineRasterizationStateCreateInfo, std::nullptr_t>;

template <GraphicsApi G>
using PipelineMultisampleStateCreateInfo =
	std::conditional_t<G == kVk, VkPipelineMultisampleStateCreateInfo, std::nullptr_t>;

template <GraphicsApi G>
using PipelineDepthStencilStateCreateInfo =
	std::conditional_t<G == kVk, VkPipelineDepthStencilStateCreateInfo, std::nullptr_t>;

template <GraphicsApi G>
using PipelineColorBlendAttachmentState =
	std::conditional_t<G == kVk, VkPipelineColorBlendAttachmentState, std::nullptr_t>;

template <GraphicsApi G>
using PipelineColorBlendStateCreateInfo =
	std::conditional_t<G == kVk, VkPipelineColorBlendStateCreateInfo, std::nullptr_t>;

template <GraphicsApi G>
using PipelineDynamicStateCreateInfo =
	std::conditional_t<G == kVk, VkPipelineDynamicStateCreateInfo, std::nullptr_t>;

template <GraphicsApi G>
using ShaderStageFlags = std::conditional_t<G == kVk, VkShaderStageFlags, std::nullptr_t>;

template <GraphicsApi G>
using ShaderStageFlagBits = std::conditional_t<G == kVk, VkShaderStageFlagBits, std::nullptr_t>;

template <GraphicsApi G>
using DynamicState = std::conditional_t<G == kVk, VkDynamicState, std::nullptr_t>;

template <GraphicsApi G>
using SamplerHandle = std::conditional_t<G == kVk, VkSampler, std::nullptr_t>;

template <GraphicsApi G>
using SamplerCreateInfo = std::conditional_t<G == kVk, VkSamplerCreateInfo, std::nullptr_t>;

template <GraphicsApi G>
using VertexInputBindingDescription =
	std::conditional_t<G == kVk, VkVertexInputBindingDescription, std::nullptr_t>;

template <GraphicsApi G>
using VertexInputRate = std::conditional_t<G == kVk, VkVertexInputRate, std::nullptr_t>;

template <GraphicsApi G>
using VertexInputAttributeDescription =
	std::conditional_t<G == kVk, VkVertexInputAttributeDescription, std::nullptr_t>;

template <GraphicsApi G>
using CommandPoolCreateFlags =
	std::conditional_t<G == kVk, VkCommandPoolCreateFlags, std::nullptr_t>;

template <GraphicsApi G>
using CommandPoolHandle = std::conditional_t<G == kVk, VkCommandPool, std::nullptr_t>;

template <GraphicsApi G>
using CommandBufferHandle = std::conditional_t<G == kVk, VkCommandBuffer, std::nullptr_t>;

template <GraphicsApi G>
using FenceHandle = std::conditional_t<G == kVk, VkFence, std::nullptr_t>;

template <GraphicsApi G>
using SemaphoreHandle = std::conditional_t<G == kVk, VkSemaphore, std::nullptr_t>;

template <GraphicsApi G>
using SemaphoreType = std::conditional_t<G == kVk, VkSemaphoreType, std::nullptr_t>;

template <GraphicsApi G>
using ClearValue = std::conditional_t<G == kVk, VkClearValue, std::nullptr_t>;

template <GraphicsApi G>
using ClearColorValue = std::conditional_t<G == kVk, VkClearColorValue, std::nullptr_t>;

template <GraphicsApi G>
using ClearDepthStencilValue =
	std::conditional_t<G == kVk, VkClearDepthStencilValue, std::nullptr_t>;

template <GraphicsApi G>
using ClearAttachment = std::conditional_t<G == kVk, VkClearAttachment, std::nullptr_t>;

template <GraphicsApi G>
using ImageSubresourceRange = std::conditional_t<G == kVk, VkImageSubresourceRange, std::nullptr_t>;

template <GraphicsApi G>
using CommandBufferBeginInfo =
	std::conditional_t<G == kVk, VkCommandBufferBeginInfo, std::nullptr_t>;

template <GraphicsApi G>
using CommandBufferInheritanceInfo =
	std::conditional_t<G == kVk, VkCommandBufferInheritanceInfo, std::nullptr_t>;

template <GraphicsApi G>
using CommandBufferInheritanceRenderingInfo =
	std::conditional_t<G == kVk, VkCommandBufferInheritanceRenderingInfoKHR, std::nullptr_t>;

template <GraphicsApi G>
using CommandBufferLevel = std::conditional_t<G == kVk, VkCommandBufferLevel, std::nullptr_t>;

template <GraphicsApi G>
using RenderPassBeginInfo = std::conditional_t<G == kVk, VkRenderPassBeginInfo, std::nullptr_t>;

template <GraphicsApi G>
using RenderingInfo = std::conditional_t<G == kVk, VkRenderingInfoKHR, std::nullptr_t>;

template <GraphicsApi G>
using SubmitInfo = std::conditional_t<G == kVk, VkSubmitInfo, std::nullptr_t>;

template <GraphicsApi G>
using TimelineSemaphoreSubmitInfo =
	std::conditional_t<G == kVk, VkTimelineSemaphoreSubmitInfo, std::nullptr_t>;

template <GraphicsApi G>
using ImageBlit = std::conditional_t<G == kVk, VkImageBlit, std::nullptr_t>;

template <GraphicsApi G>
using Filter = std::conditional_t<G == kVk, VkFilter, std::nullptr_t>;

template <GraphicsApi G>
using ImageSubresourceLayers =
	std::conditional_t<G == kVk, VkImageSubresourceLayers, std::nullptr_t>;

template <GraphicsApi G>
using PresentInfo = std::conditional_t<G == kVk, VkPresentInfoKHR, std::nullptr_t>;

template <GraphicsApi G>
using DescriptorType = std::conditional_t<G == kVk, VkDescriptorType, std::nullptr_t>;

template <GraphicsApi G>
using DescriptorBufferInfo = std::conditional_t<G == kVk, VkDescriptorBufferInfo, std::nullptr_t>;

template <GraphicsApi G>
using DescriptorImageInfo = std::conditional_t<G == kVk, VkDescriptorImageInfo, std::nullptr_t>;

template <GraphicsApi G>
using AccelerationStructureHandle =
	std::conditional_t<G == kVk, VkAccelerationStructureKHR, std::nullptr_t>;

template <GraphicsApi G>
using CopyDescriptorSet = std::conditional_t<G == kVk, VkCopyDescriptorSet, std::nullptr_t>;

template <GraphicsApi G>
using WriteDescriptorSet = std::conditional_t<G == kVk, VkWriteDescriptorSet, std::nullptr_t>;

template <GraphicsApi G>
using InlineUniformBlock =
	std::conditional_t<G == kVk, VkWriteDescriptorSetInlineUniformBlockEXT, std::nullptr_t>;

template <GraphicsApi G>
using PushConstantRange = std::conditional_t<G == kVk, VkPushConstantRange, std::nullptr_t>;

template <GraphicsApi G>
using DescriptorBindingFlags =
	std::conditional_t<G == kVk, VkDescriptorBindingFlags, std::nullptr_t>;

template <GraphicsApi G>
using ImageAspectFlags =
	std::conditional_t<G == kVk, VkImageAspectFlags, std::nullptr_t>;

template <GraphicsApi G>
using PipelineRenderingCreateInfo = 
	std::conditional_t<G == kVk, VkPipelineRenderingCreateInfoKHR, std::nullptr_t>;

template <GraphicsApi G>
using RenderingAttachmentInfo = 
	std::conditional_t<G == kVk, VkRenderingAttachmentInfoKHR, std::nullptr_t>;
	