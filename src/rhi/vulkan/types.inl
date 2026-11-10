// the Vulkan backend's types (see BackendTypes in rhi/types.h): what rhi's interface names of the backend, which the
// backend's own sources may name directly
#include <rhi/capi.h>

#include <vulkan/vulkan.h>
#include <vulkan/vk_enum_string_helper.h>

#if defined(__WINDOWS__)
#include <vma/vk_mem_alloc.h> // no windows.h/vulkan_win32.h: no Win32-specific Vulkan types are used (surfaces come from glfw)
#elif defined(__LINUX__)
#include <vk_mem_alloc.h>
#include <vulkan/vulkan_wayland.h>
#elif defined(__OSX__)
#include <vk_mem_alloc.h>
#include <vulkan/vulkan_beta.h>
#include <vulkan/vulkan_metal.h>
#endif

template <>
struct BackendTypes<kVk>
{
	using StructureType = VkStructureType;
	using AllocationCallbacks = VkAllocationCallbacks;
	using ApplicationInfo = VkApplicationInfo;
	using Result = VkResult;
	using Flags = VkFlags;
	using DeviceSize = VkDeviceSize;
	// the backend's viewport and scissor, for state kept in the backend's own layout (see the neutral Viewport and Rect)
	using NativeViewport = VkViewport;
	using NativeRect = VkRect2D;
	using ObjectType = VkObjectType;
	using InstanceHandle = VkInstance;
	using BufferHandle = VkBuffer;
	using ImageHandle = VkImage;
	using AllocatorHandle = VmaAllocator;
	using AllocationHandle = VmaAllocation;
	using BufferViewHandle = VkBufferView;
	using ImageViewHandle = VkImageView;
	using SurfaceHandle = VkSurfaceKHR;
	using SurfaceFormat = VkSurfaceFormatKHR;
	using PresentMode = VkPresentModeKHR;
	using SurfaceCapabilities = VkSurfaceCapabilitiesKHR;
	using QueueFamilyProperties = VkQueueFamilyProperties;
	using SwapchainHandle = VkSwapchainKHR;
	using FramebufferHandle = VkFramebuffer;
	using AttachmentDescription = VkAttachmentDescription2;
	using DescriptorSetLayoutBinding = VkDescriptorSetLayoutBinding;
	using DescriptorSetLayoutCreateFlags = VkDescriptorSetLayoutCreateFlags;
	using DeviceHandle = VkDevice;
	using PhysicalDeviceHandle = VkPhysicalDevice;
	using PhysicalDeviceProperties = VkPhysicalDeviceProperties2;
	// the core versions' (see kVulkanApiVersion): what was an extension's before is queried and enabled here
	using PhysicalDeviceProperties11Ex = VkPhysicalDeviceVulkan11Properties;
	using PhysicalDeviceProperties12Ex = VkPhysicalDeviceVulkan12Properties;
	using PhysicalDeviceProperties13Ex = VkPhysicalDeviceVulkan13Properties;
	using PhysicalDeviceProperties14Ex = VkPhysicalDeviceVulkan14Properties;
	using PhysicalDeviceFeatures = VkPhysicalDeviceFeatures2;
	using PhysicalDeviceFeatures11Ex = VkPhysicalDeviceVulkan11Features;
	using PhysicalDeviceFeatures12Ex = VkPhysicalDeviceVulkan12Features;
	using PhysicalDeviceFeatures13Ex = VkPhysicalDeviceVulkan13Features;
	using PhysicalDeviceFeatures14Ex = VkPhysicalDeviceVulkan14Features;
	// the extensions' (not core in kVulkanApiVersion)
	using PhysicalDevicePresentIdFeatures = VkPhysicalDevicePresentIdFeaturesKHR;
	using PhysicalDevicePresentWaitFeatures = VkPhysicalDevicePresentWaitFeaturesKHR;
	using PhysicalDeviceSwapchainMaintenance1Features = VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR;
	// crash reports' (see rhi/vulkan/crashreport.h)
	using PhysicalDeviceFaultFeatures = VkPhysicalDeviceFaultFeaturesEXT;
	using PhysicalDeviceAddressBindingReportFeatures = VkPhysicalDeviceAddressBindingReportFeaturesEXT;
	using PhysicalDeviceCoherentMemoryFeatures = VkPhysicalDeviceCoherentMemoryFeaturesAMD;
	using PhysicalDeviceAccelerationStructureFeatures = VkPhysicalDeviceAccelerationStructureFeaturesKHR;
	using PhysicalDeviceRayQueryFeatures = VkPhysicalDeviceRayQueryFeaturesKHR;
	using PhysicalDeviceAccelerationStructureProperties = VkPhysicalDeviceAccelerationStructurePropertiesKHR;
	using QueueHandle = VkQueue;
	using CommandBufferHandle = VkCommandBuffer;
	using ShaderModuleHandle = VkShaderModule;
	using DescriptorUpdateTemplateEntry = VkDescriptorUpdateTemplateEntry;
	using DescriptorUpdateTemplateType = VkDescriptorUpdateTemplateType;
	using DescriptorSetLayoutHandle = VkDescriptorSetLayout;
	using DescriptorUpdateTemplateHandle = VkDescriptorUpdateTemplate;
	using PipelineLayoutHandle = VkPipelineLayout;
	using DescriptorPoolHandle = VkDescriptorPool;
	using DescriptorSetHandle = VkDescriptorSet;
	using RenderPassHandle = VkRenderPass;
	using PipelineHandle = VkPipeline;
	using PipelineCacheHandle = VkPipelineCache;
	using PipelineShaderStageCreateInfo = VkPipelineShaderStageCreateInfo;
	using PipelineVertexInputStateCreateInfo = VkPipelineVertexInputStateCreateInfo;
	using PipelineInputAssemblyStateCreateInfo = VkPipelineInputAssemblyStateCreateInfo;
	using PipelineViewportStateCreateInfo = VkPipelineViewportStateCreateInfo;
	using PipelineRasterizationStateCreateInfo = VkPipelineRasterizationStateCreateInfo;
	using PipelineMultisampleStateCreateInfo = VkPipelineMultisampleStateCreateInfo;
	using PipelineDepthStencilStateCreateInfo = VkPipelineDepthStencilStateCreateInfo;
	using PipelineColorBlendAttachmentState = VkPipelineColorBlendAttachmentState;
	using PipelineColorBlendStateCreateInfo = VkPipelineColorBlendStateCreateInfo;
	using PipelineDynamicStateCreateInfo = VkPipelineDynamicStateCreateInfo;
	using DynamicState = VkDynamicState;
	using SamplerHandle = VkSampler;
	using CommandPoolCreateFlags = VkCommandPoolCreateFlags;
	using CommandPoolHandle = VkCommandPool;
	using FenceHandle = VkFence;
	using QueryPoolHandle = VkQueryPool;
	using SemaphoreHandle = VkSemaphore;
	using ImageSubresourceRange = VkImageSubresourceRange;
	using CommandBufferInheritanceRenderingInfo = VkCommandBufferInheritanceRenderingInfoKHR;
	using RenderPassBeginInfo = VkRenderPassBeginInfo;
	using RenderingInfo = VkRenderingInfoKHR;
	using ImageSubresourceLayers = VkImageSubresourceLayers;
	using DescriptorType = VkDescriptorType;
	using DescriptorBufferInfo = VkDescriptorBufferInfo;
	using DescriptorImageInfo = VkDescriptorImageInfo;
	using AccelerationStructureHandle = VkAccelerationStructureKHR;
	using PushConstantRange = VkPushConstantRange;
	using DescriptorBindingFlags = VkDescriptorBindingFlags;
	using PipelineRenderingCreateInfo = VkPipelineRenderingCreateInfoKHR;
};
