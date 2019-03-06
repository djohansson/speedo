#pragma once

#include <cstdint>
#include <type_traits>

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

enum class GraphicsBackend
{
	Vulkan
};

template <GraphicsBackend Backend>
using Image = std::conditional_t<Backend == GraphicsBackend::Vulkan, VkImage, std::nullptr_t>;

template <GraphicsBackend Backend>
using Allocation = std::conditional_t<Backend == GraphicsBackend::Vulkan, VmaAllocation, std::nullptr_t>;

template <GraphicsBackend Backend>
using ImageView = std::conditional_t<Backend == GraphicsBackend::Vulkan, VkImageView, std::nullptr_t>;

template <GraphicsBackend Backend>
using Surface = std::conditional_t<Backend == GraphicsBackend::Vulkan, VkSurfaceKHR, std::nullptr_t>;

template <GraphicsBackend Backend>
using SurfaceFormat = std::conditional_t<Backend == GraphicsBackend::Vulkan, VkSurfaceFormatKHR, std::nullptr_t>;

template <GraphicsBackend Backend>
using Format = std::conditional_t<Backend == GraphicsBackend::Vulkan, VkFormat, std::nullptr_t>;

template <GraphicsBackend Backend>
using PresentMode = std::conditional_t<Backend == GraphicsBackend::Vulkan, VkPresentModeKHR, std::nullptr_t>;

template <GraphicsBackend Backend>
using Buffer = std::conditional_t<Backend == GraphicsBackend::Vulkan, VkBuffer, std::nullptr_t>;

template <GraphicsBackend Backend>
using SurfaceCapabilities = std::conditional_t<Backend == GraphicsBackend::Vulkan, VkSurfaceCapabilitiesKHR, std::nullptr_t>;

template <GraphicsBackend Backend>
using SurfaceFormat = std::conditional_t<Backend == GraphicsBackend::Vulkan, VkSurfaceFormatKHR, std::nullptr_t>;

template <GraphicsBackend Backend>
using PresentMode = std::conditional_t<Backend == GraphicsBackend::Vulkan, VkPresentModeKHR, std::nullptr_t>;

template <GraphicsBackend Backend>
using Swapchain = std::conditional_t<Backend == GraphicsBackend::Vulkan, VkSwapchainKHR, std::nullptr_t>;

template <GraphicsBackend Backend>
using Framebuffer = std::conditional_t<Backend == GraphicsBackend::Vulkan, VkFramebuffer, std::nullptr_t>;

template <GraphicsBackend Backend>
using DescriptorSetLayoutBinding = std::conditional_t<Backend == GraphicsBackend::Vulkan, VkDescriptorSetLayoutBinding, std::nullptr_t>;

template <GraphicsBackend Backend>
using ShaderModule = std::conditional_t<Backend == GraphicsBackend::Vulkan, VkShaderModule, std::nullptr_t>;

template <GraphicsBackend Backend>
using DescriptorSetLayout = std::conditional_t<Backend == GraphicsBackend::Vulkan, VkDescriptorSetLayout, std::nullptr_t>;

template <GraphicsBackend Backend>
using PipelineLayout = std::conditional_t<Backend == GraphicsBackend::Vulkan, VkPipelineLayout, std::nullptr_t>;

template <GraphicsBackend Backend>
using DescriptorSet = std::conditional_t<Backend == GraphicsBackend::Vulkan, VkDescriptorSet, std::nullptr_t>;

template <GraphicsBackend Backend>
using RenderPass = std::conditional_t<Backend == GraphicsBackend::Vulkan, VkRenderPass, std::nullptr_t>;

template <GraphicsBackend Backend>
using Pipeline = std::conditional_t<Backend == GraphicsBackend::Vulkan, VkPipeline, std::nullptr_t>;

template <GraphicsBackend Backend>
using Sampler = std::conditional_t<Backend == GraphicsBackend::Vulkan, VkSampler, std::nullptr_t>;

template <GraphicsBackend Backend>
struct Texture
{
	Image<Backend> image;
	Allocation<Backend> imageMemory;
	ImageView<Backend> imageView;
};

template <GraphicsBackend Backend>
struct Model
{
	Buffer<Backend> vertexBuffer;
	Allocation<Backend> vertexBufferMemory;
	Buffer<Backend> indexBuffer;
	Allocation<Backend> indexBufferMemory;
	uint32_t indexCount;
};

template <GraphicsBackend Backend>
struct SwapchainInfo
{
	SurfaceCapabilities<Backend> capabilities;
	std::vector<SurfaceFormat<Backend>> formats;
	std::vector<PresentMode<Backend>> presentModes;
};

template <GraphicsBackend Backend>
struct SwapchainContext
{
    SwapchainInfo<Backend> info;
	Swapchain<Backend> swapchain;

	std::vector<Framebuffer<Backend>> frameBuffers;

	std::vector<Image<Backend>> colorImages;
	std::vector<ImageView<Backend>> colorImageViews;

	Image<Backend> depthImage;
	Allocation<Backend> depthImageMemory;
	ImageView<Backend> depthImageView;
};
