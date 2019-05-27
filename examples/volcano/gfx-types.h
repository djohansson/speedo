#pragma once

#include "utils.h"

#include <cstdint>
#include <type_traits>

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>


enum class GraphicsBackend
{
	Vulkan
};

template <GraphicsBackend B>
using Instance = std::conditional_t<B == GraphicsBackend::Vulkan, VkInstance, std::nullptr_t>;

template <GraphicsBackend B>
using Image = std::conditional_t<B == GraphicsBackend::Vulkan, VkImage, std::nullptr_t>;

template <GraphicsBackend B>
using Allocation = std::conditional_t<B == GraphicsBackend::Vulkan, VmaAllocation, std::nullptr_t>;

template <GraphicsBackend B>
using ImageView = std::conditional_t<B == GraphicsBackend::Vulkan, VkImageView, std::nullptr_t>;

template <GraphicsBackend B>
using Surface = std::conditional_t<B == GraphicsBackend::Vulkan, VkSurfaceKHR, std::nullptr_t>;

template <GraphicsBackend B>
using SurfaceFormat =
	std::conditional_t<B == GraphicsBackend::Vulkan, VkSurfaceFormatKHR, std::nullptr_t>;

template <GraphicsBackend B>
using Format = std::conditional_t<B == GraphicsBackend::Vulkan, VkFormat, std::nullptr_t>;

template <GraphicsBackend B>
using PresentMode =
	std::conditional_t<B == GraphicsBackend::Vulkan, VkPresentModeKHR, std::nullptr_t>;

template <GraphicsBackend B>
using Buffer = std::conditional_t<B == GraphicsBackend::Vulkan, VkBuffer, std::nullptr_t>;

template <GraphicsBackend B>
using SurfaceCapabilities =
	std::conditional_t<B == GraphicsBackend::Vulkan, VkSurfaceCapabilitiesKHR, std::nullptr_t>;

template <GraphicsBackend B>
using SurfaceFormat =
	std::conditional_t<B == GraphicsBackend::Vulkan, VkSurfaceFormatKHR, std::nullptr_t>;

template <GraphicsBackend B>
using PresentMode =
	std::conditional_t<B == GraphicsBackend::Vulkan, VkPresentModeKHR, std::nullptr_t>;

template <GraphicsBackend B>
using Swapchain = std::conditional_t<B == GraphicsBackend::Vulkan, VkSwapchainKHR, std::nullptr_t>;

template <GraphicsBackend B>
using Framebuffer = std::conditional_t<B == GraphicsBackend::Vulkan, VkFramebuffer, std::nullptr_t>;

template <GraphicsBackend B>
using DescriptorSetLayoutBinding =
	std::conditional_t<B == GraphicsBackend::Vulkan, VkDescriptorSetLayoutBinding, std::nullptr_t>;

template <GraphicsBackend B>
using Device = std::conditional_t<B == GraphicsBackend::Vulkan, VkDevice, std::nullptr_t>;

template <GraphicsBackend B>
using PhysicalDevice =
	std::conditional_t<B == GraphicsBackend::Vulkan, VkPhysicalDevice, std::nullptr_t>;

template <GraphicsBackend B>
using PhysicalDeviceProperties =
	std::conditional_t<B == GraphicsBackend::Vulkan, VkPhysicalDeviceProperties, std::nullptr_t>;

template <GraphicsBackend B>
using Queue = std::conditional_t<B == GraphicsBackend::Vulkan, VkQueue, std::nullptr_t>;

template <GraphicsBackend B>
using ShaderModule =
	std::conditional_t<B == GraphicsBackend::Vulkan, VkShaderModule, std::nullptr_t>;

template <GraphicsBackend B>
using DescriptorSetLayout =
	std::conditional_t<B == GraphicsBackend::Vulkan, VkDescriptorSetLayout, std::nullptr_t>;

template <GraphicsBackend B>
using PipelineLayout =
	std::conditional_t<B == GraphicsBackend::Vulkan, VkPipelineLayout, std::nullptr_t>;

template <GraphicsBackend B>
struct PipelineLayoutContext
{
	std::unique_ptr<ShaderModule<B>[], ArrayDeleter<ShaderModule<B>>> shaders;
	std::unique_ptr<DescriptorSetLayout<B>[], ArrayDeleter<DescriptorSetLayout<B>>>
		descriptorSetLayouts;

	PipelineLayout<B> layout;
};

template <GraphicsBackend B>
using DescriptorPool = std::conditional_t<B == GraphicsBackend::Vulkan, VkDescriptorPool, std::nullptr_t>;

template <GraphicsBackend B>
using DescriptorSet =
	std::conditional_t<B == GraphicsBackend::Vulkan, VkDescriptorSet, std::nullptr_t>;

template <GraphicsBackend B>
using RenderPass = std::conditional_t<B == GraphicsBackend::Vulkan, VkRenderPass, std::nullptr_t>;

template <GraphicsBackend B>
using Pipeline = std::conditional_t<B == GraphicsBackend::Vulkan, VkPipeline, std::nullptr_t>;

template <GraphicsBackend B>
using PipelineCache = std::conditional_t<B == GraphicsBackend::Vulkan, VkPipelineCache, std::nullptr_t>;

template <GraphicsBackend B>
using Sampler = std::conditional_t<B == GraphicsBackend::Vulkan, VkSampler, std::nullptr_t>;

template <GraphicsBackend B>
struct Texture
{
	Image<B> image;
	Allocation<B> imageMemory;
	ImageView<B> imageView;
};

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
struct SerializableVertexInputAttributeDescription : public VertexInputAttributeDescription<B>
{
	using BaseType = VertexInputAttributeDescription<B>;

	template <class Archive, GraphicsBackend B = B>
	typename std::enable_if_t<B == GraphicsBackend::Vulkan, void> serialize(Archive& ar)
	{
		static_assert(sizeof(*this) == sizeof(BaseType));

		ar(BaseType::location);
		ar(BaseType::binding);
		ar(BaseType::format);
		ar(BaseType::offset);
	}
};

template <GraphicsBackend B>
struct Model
{
	Buffer<B> vertexBuffer;
	Allocation<B> vertexBufferMemory;
	Buffer<B> indexBuffer;
	Allocation<B> indexBufferMemory;
	uint32_t indexCount;

	std::vector<VertexInputAttributeDescription<B>> attributeDescriptions;
	std::vector<VertexInputBindingDescription<B>> bindingDescriptions;
};

template <GraphicsBackend B>
struct SwapchainInfo
{
	SurfaceCapabilities<B> capabilities;
	std::vector<SurfaceFormat<B>> formats;
	std::vector<PresentMode<B>> presentModes;
};

template <GraphicsBackend B>
struct SwapchainContext
{
	SwapchainInfo<B> info;
	Swapchain<B> swapchain;

	std::vector<Framebuffer<B>> frameBuffers;

	std::vector<Image<B>> colorImages;
	std::vector<ImageView<B>> colorImageViews;

	Image<B> depthImage;
	Allocation<B> depthImageMemory;
	ImageView<B> depthImageView;
};
