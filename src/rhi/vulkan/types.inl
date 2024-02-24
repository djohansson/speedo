#include <vma/vk_mem_alloc.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_beta.h>

template <GraphicsApi G>
using AllocationCallbacks = std::conditional_t<G == Vk, VkAllocationCallbacks, std::nullptr_t>;

template <GraphicsApi G>
using ApplicationInfo = std::conditional_t<G == Vk, VkApplicationInfo, std::nullptr_t>;

template <GraphicsApi G>
using SystemAllocationScope  = std::conditional_t<G == Vk, VkSystemAllocationScope, std::nullptr_t>;;

template <GraphicsApi G>
using ObjectInfo = std::conditional_t<G == Vk, VkDebugUtilsObjectNameInfoEXT, std::nullptr_t>;

template <GraphicsApi G>
using Result = std::conditional_t<G == Vk, VkResult, std::nullptr_t>;

template <GraphicsApi G>
using Flags = std::conditional_t<G == Vk, VkFlags, std::nullptr_t>;

template <GraphicsApi G>
using DeviceSize = std::conditional_t<G == Vk, VkDeviceSize, std::nullptr_t>;

template <GraphicsApi G>
using Extent2d = std::conditional_t<G == Vk, VkExtent2D, std::nullptr_t>;

template <GraphicsApi G>
using Extent3d = std::conditional_t<G == Vk, VkExtent3D, std::nullptr_t>;

template <GraphicsApi G>
using Viewport = std::conditional_t<G == Vk, VkViewport, std::nullptr_t>;

template <GraphicsApi G>
using Rect2D = std::conditional_t<G == Vk, VkRect2D, std::nullptr_t>;

template <GraphicsApi G>
using ObjectType = std::conditional_t<G == Vk, VkObjectType, std::nullptr_t>;

template <GraphicsApi G>
using InstanceHandle = std::conditional_t<G == Vk, VkInstance, std::nullptr_t>;

template <GraphicsApi G>
using BufferHandle = std::conditional_t<G == Vk, VkBuffer, std::nullptr_t>;

template <GraphicsApi G>
using ImageHandle = std::conditional_t<G == Vk, VkImage, std::nullptr_t>;

template <GraphicsApi G>
using ImageLayout = std::conditional_t<G == Vk, VkImageLayout, std::nullptr_t>;

template <GraphicsApi G>
using ImageTiling = std::conditional_t<G == Vk, VkImageTiling, std::nullptr_t>;

template <GraphicsApi G>
using AllocatorHandle = std::conditional_t<G == Vk, VmaAllocator, std::nullptr_t>;

template <GraphicsApi G>
using AllocationHandle = std::conditional_t<G == Vk, VmaAllocation, std::nullptr_t>;

template <GraphicsApi G>
using BufferViewHandle = std::conditional_t<G == Vk, VkBufferView, std::nullptr_t>;

template <GraphicsApi G>
using ImageViewHandle = std::conditional_t<G == Vk, VkImageView, std::nullptr_t>;

template <GraphicsApi G>
using SurfaceHandle = std::conditional_t<G == Vk, VkSurfaceKHR, std::nullptr_t>;

template <GraphicsApi G>
using SurfaceFormat = std::conditional_t<G == Vk, VkSurfaceFormatKHR, std::nullptr_t>;

template <GraphicsApi G>
using Format = std::conditional_t<G == Vk, VkFormat, std::nullptr_t>;

template <GraphicsApi G>
using ColorSpace = std::conditional_t<G == Vk, VkColorSpaceKHR, std::nullptr_t>;

template <GraphicsApi G>
using PresentMode = std::conditional_t<G == Vk, VkPresentModeKHR, std::nullptr_t>;

template <GraphicsApi G>
using SurfaceCapabilities = std::conditional_t<G == Vk, VkSurfaceCapabilitiesKHR, std::nullptr_t>;

template <GraphicsApi G>
using QueueFamilyProperties = std::conditional_t<G == Vk, VkQueueFamilyProperties, std::nullptr_t>;

template <GraphicsApi G>
using SwapchainHandle = std::conditional_t<G == Vk, VkSwapchainKHR, std::nullptr_t>;

template <GraphicsApi G>
using FramebufferHandle = std::conditional_t<G == Vk, VkFramebuffer, std::nullptr_t>;

template <GraphicsApi G>
using AttachmentDescription = std::conditional_t<G == Vk, VkAttachmentDescription, std::nullptr_t>;

template <GraphicsApi G>
using AttachmentReference = std::conditional_t<G == Vk, VkAttachmentReference, std::nullptr_t>;

template <GraphicsApi G>
using SubpassDescription = std::conditional_t<G == Vk, VkSubpassDescription, std::nullptr_t>;

template <GraphicsApi G>
using SubpassDependency = std::conditional_t<G == Vk, VkSubpassDependency, std::nullptr_t>;

template <GraphicsApi G>
using AttachmentLoadOp = std::conditional_t<G == Vk, VkAttachmentLoadOp, std::nullptr_t>;

template <GraphicsApi G>
using AttachmentStoreOp = std::conditional_t<G == Vk, VkAttachmentStoreOp, std::nullptr_t>;

template <GraphicsApi G>
using DescriptorSetLayoutBinding =
	std::conditional_t<G == Vk, VkDescriptorSetLayoutBinding, std::nullptr_t>;

template <GraphicsApi G>
using DescriptorSetLayoutCreateFlags =
	std::conditional_t<G == Vk, VkDescriptorSetLayoutCreateFlags, std::nullptr_t>;

template <GraphicsApi G>
using DeviceHandle = std::conditional_t<G == Vk, VkDevice, std::nullptr_t>;

template <GraphicsApi G>
using PhysicalDeviceHandle = std::conditional_t<G == Vk, VkPhysicalDevice, std::nullptr_t>;

template <GraphicsApi G>
using PhysicalDeviceProperties =
	std::conditional_t<G == Vk, VkPhysicalDeviceProperties2, std::nullptr_t>;

template <GraphicsApi G>
using PhysicalDevicePropertiesEx =
	std::conditional_t<G == Vk, VkPhysicalDeviceVulkan12Properties, std::nullptr_t>;

template <GraphicsApi G>
using PhysicalDeviceFeatures =
	std::conditional_t<G == Vk, VkPhysicalDeviceFeatures2, std::nullptr_t>;

template <GraphicsApi G>
using PhysicalDeviceFeaturesEx =
	std::conditional_t<G == Vk, VkPhysicalDeviceVulkan12Features, std::nullptr_t>;

template <GraphicsApi G>
using PhysicalDeviceRobustnessFeatures =
	std::conditional_t<G == Vk, VkPhysicalDeviceRobustness2FeaturesEXT, std::nullptr_t>;

template <GraphicsApi G>
using QueueHandle = std::conditional_t<G == Vk, VkQueue, std::nullptr_t>;

template <GraphicsApi G>
using CommandBufferHandle = std::conditional_t<G == Vk, VkCommandBuffer, std::nullptr_t>;

template <GraphicsApi G>
using ShaderModuleHandle = std::conditional_t<G == Vk, VkShaderModule, std::nullptr_t>;

template <GraphicsApi G>
using DescriptorUpdateTemplateEntry =
	std::conditional_t<G == Vk, VkDescriptorUpdateTemplateEntry, std::nullptr_t>;

template <GraphicsApi G>
using DescriptorUpdateTemplateType =
	std::conditional_t<G == Vk, VkDescriptorUpdateTemplateType, std::nullptr_t>;

template <GraphicsApi G>
using DescriptorSetLayoutHandle =
	std::conditional_t<G == Vk, VkDescriptorSetLayout, std::nullptr_t>;

template <GraphicsApi G>
using DescriptorUpdateTemplateHandle =
	std::conditional_t<G == Vk, VkDescriptorUpdateTemplate, std::nullptr_t>;

template <GraphicsApi G>
using PipelineLayoutHandle = std::conditional_t<G == Vk, VkPipelineLayout, std::nullptr_t>;

template <GraphicsApi G>
using PipelineBindPoint = std::conditional_t<G == Vk, VkPipelineBindPoint, std::nullptr_t>;

template <GraphicsApi G>
using DescriptorPoolHandle = std::conditional_t<G == Vk, VkDescriptorPool, std::nullptr_t>;

template <GraphicsApi G>
using DescriptorSetHandle = std::conditional_t<G == Vk, VkDescriptorSet, std::nullptr_t>;

template <GraphicsApi G>
using RenderPassHandle = std::conditional_t<G == Vk, VkRenderPass, std::nullptr_t>;

template <GraphicsApi G>
using SubpassContents = std::conditional_t<G == Vk, VkSubpassContents, std::nullptr_t>;

template <GraphicsApi G>
using PipelineHandle = std::conditional_t<G == Vk, VkPipeline, std::nullptr_t>;

template <GraphicsApi G>
using PipelineCacheHandle = std::conditional_t<G == Vk, VkPipelineCache, std::nullptr_t>;

template <GraphicsApi G>
using PipelineShaderStageCreateInfo =
	std::conditional_t<G == Vk, VkPipelineShaderStageCreateInfo, std::nullptr_t>;

template <GraphicsApi G>
using PipelineVertexInputStateCreateInfo =
	std::conditional_t<G == Vk, VkPipelineVertexInputStateCreateInfo, std::nullptr_t>;

template <GraphicsApi G>
using PipelineInputAssemblyStateCreateInfo =
	std::conditional_t<G == Vk, VkPipelineInputAssemblyStateCreateInfo, std::nullptr_t>;

template <GraphicsApi G>
using PipelineViewportStateCreateInfo =
	std::conditional_t<G == Vk, VkPipelineViewportStateCreateInfo, std::nullptr_t>;

template <GraphicsApi G>
using PipelineRasterizationStateCreateInfo =
	std::conditional_t<G == Vk, VkPipelineRasterizationStateCreateInfo, std::nullptr_t>;

template <GraphicsApi G>
using PipelineMultisampleStateCreateInfo =
	std::conditional_t<G == Vk, VkPipelineMultisampleStateCreateInfo, std::nullptr_t>;

template <GraphicsApi G>
using PipelineDepthStencilStateCreateInfo =
	std::conditional_t<G == Vk, VkPipelineDepthStencilStateCreateInfo, std::nullptr_t>;

template <GraphicsApi G>
using PipelineColorBlendAttachmentState =
	std::conditional_t<G == Vk, VkPipelineColorBlendAttachmentState, std::nullptr_t>;

template <GraphicsApi G>
using PipelineColorBlendStateCreateInfo =
	std::conditional_t<G == Vk, VkPipelineColorBlendStateCreateInfo, std::nullptr_t>;

template <GraphicsApi G>
using PipelineDynamicStateCreateInfo =
	std::conditional_t<G == Vk, VkPipelineDynamicStateCreateInfo, std::nullptr_t>;

template <GraphicsApi G>
using ShaderStageFlags = std::conditional_t<G == Vk, VkShaderStageFlags, std::nullptr_t>;

template <GraphicsApi G>
using ShaderStageFlagBits = std::conditional_t<G == Vk, VkShaderStageFlagBits, std::nullptr_t>;

template <GraphicsApi G>
using DynamicState = std::conditional_t<G == Vk, VkDynamicState, std::nullptr_t>;

template <GraphicsApi G>
using SamplerHandle = std::conditional_t<G == Vk, VkSampler, std::nullptr_t>;

template <GraphicsApi G>
using SamplerCreateInfo = std::conditional_t<G == Vk, VkSamplerCreateInfo, std::nullptr_t>;

template <GraphicsApi G>
using VertexInputBindingDescription =
	std::conditional_t<G == Vk, VkVertexInputBindingDescription, std::nullptr_t>;

template <GraphicsApi G>
using VertexInputRate = std::conditional_t<G == Vk, VkVertexInputRate, std::nullptr_t>;

template <GraphicsApi G>
using VertexInputAttributeDescription =
	std::conditional_t<G == Vk, VkVertexInputAttributeDescription, std::nullptr_t>;

template <GraphicsApi G>
using CommandPoolCreateFlags =
	std::conditional_t<G == Vk, VkCommandPoolCreateFlags, std::nullptr_t>;

template <GraphicsApi G>
using CommandPoolHandle = std::conditional_t<G == Vk, VkCommandPool, std::nullptr_t>;

template <GraphicsApi G>
using CommandBufferHandle = std::conditional_t<G == Vk, VkCommandBuffer, std::nullptr_t>;

template <GraphicsApi G>
using FenceHandle = std::conditional_t<G == Vk, VkFence, std::nullptr_t>;

template <GraphicsApi G>
using SemaphoreHandle = std::conditional_t<G == Vk, VkSemaphore, std::nullptr_t>;

template <GraphicsApi G>
using ClearValue = std::conditional_t<G == Vk, VkClearValue, std::nullptr_t>;

template <GraphicsApi G>
using ClearColorValue = std::conditional_t<G == Vk, VkClearColorValue, std::nullptr_t>;

template <GraphicsApi G>
using ClearDepthStencilValue =
	std::conditional_t<G == Vk, VkClearDepthStencilValue, std::nullptr_t>;

template <GraphicsApi G>
using ClearAttachment = std::conditional_t<G == Vk, VkClearAttachment, std::nullptr_t>;

template <GraphicsApi G>
using ImageSubresourceRange = std::conditional_t<G == Vk, VkImageSubresourceRange, std::nullptr_t>;

template <GraphicsApi G>
using CommandBufferBeginInfo =
	std::conditional_t<G == Vk, VkCommandBufferBeginInfo, std::nullptr_t>;

template <GraphicsApi G>
using CommandBufferInheritanceInfo =
	std::conditional_t<G == Vk, VkCommandBufferInheritanceInfo, std::nullptr_t>;

template <GraphicsApi G>
using CommandBufferLevel = std::conditional_t<G == Vk, VkCommandBufferLevel, std::nullptr_t>;

template <GraphicsApi G>
using RenderPassBeginInfo = std::conditional_t<G == Vk, VkRenderPassBeginInfo, std::nullptr_t>;

template <GraphicsApi G>
using SubmitInfo = std::conditional_t<G == Vk, VkSubmitInfo, std::nullptr_t>;

template <GraphicsApi G>
using TimelineSemaphoreSubmitInfo =
	std::conditional_t<G == Vk, VkTimelineSemaphoreSubmitInfo, std::nullptr_t>;

template <GraphicsApi G>
using ImageBlit = std::conditional_t<G == Vk, VkImageBlit, std::nullptr_t>;

template <GraphicsApi G>
using Filter = std::conditional_t<G == Vk, VkFilter, std::nullptr_t>;

template <GraphicsApi G>
using ImageSubresourceLayers =
	std::conditional_t<G == Vk, VkImageSubresourceLayers, std::nullptr_t>;

template <GraphicsApi G>
using PresentInfo = std::conditional_t<G == Vk, VkPresentInfoKHR, std::nullptr_t>;

template <GraphicsApi G>
using DescriptorType = std::conditional_t<G == Vk, VkDescriptorType, std::nullptr_t>;

template <GraphicsApi G>
using DescriptorBufferInfo = std::conditional_t<G == Vk, VkDescriptorBufferInfo, std::nullptr_t>;

template <GraphicsApi G>
using DescriptorImageInfo = std::conditional_t<G == Vk, VkDescriptorImageInfo, std::nullptr_t>;

template <GraphicsApi G>
using AccelerationStructureHandle =
	std::conditional_t<G == Vk, VkAccelerationStructureKHR, std::nullptr_t>;

template <GraphicsApi G>
using CopyDescriptorSet = std::conditional_t<G == Vk, VkCopyDescriptorSet, std::nullptr_t>;

template <GraphicsApi G>
using WriteDescriptorSet = std::conditional_t<G == Vk, VkWriteDescriptorSet, std::nullptr_t>;

template <GraphicsApi G>
using InlineUniformBlock =
	std::conditional_t<G == Vk, VkWriteDescriptorSetInlineUniformBlockEXT, std::nullptr_t>;

template <GraphicsApi G>
using PushConstantRange = std::conditional_t<G == Vk, VkPushConstantRange, std::nullptr_t>;

template <GraphicsApi G>
using DescriptorBindingFlags =
	std::conditional_t<G == Vk, VkDescriptorBindingFlags, std::nullptr_t>;
