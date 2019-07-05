#include "gfx.h"

#include <algorithm>
#include <tuple>
#include <vector>

#define VMA_IMPLEMENTATION
#ifdef _DEBUG
#	define VMA_DEBUG_INITIALIZE_ALLOCATIONS 1
#	define VMA_DEBUG_MARGIN 16
#	define VMA_DEBUG_DETECT_CORRUPTION 1
#endif
#include <vk_mem_alloc.h>

#pragma pack(push, 1)
template <>
struct PipelineCacheHeader<GraphicsBackend::Vulkan>
{
	uint32_t headerLength = 0;
	uint32_t cacheHeaderVersion = 0;
	uint32_t vendorID = 0;
	uint32_t deviceID = 0;
	uint8_t pipelineCacheUUID[VK_UUID_SIZE] = {};
};
#pragma pack(pop)

template <>
bool isCacheValid(const PipelineCacheHeader<GraphicsBackend::Vulkan>& header, const PhysicalDeviceProperties<GraphicsBackend::Vulkan>& physicalDeviceProperties)
{
	return (header.headerLength > 0 &&
		header.cacheHeaderVersion == VK_PIPELINE_CACHE_HEADER_VERSION_ONE &&
		header.vendorID == physicalDeviceProperties.vendorID &&
		header.deviceID == physicalDeviceProperties.deviceID &&
		memcmp(header.pipelineCacheUUID, physicalDeviceProperties.pipelineCacheUUID, sizeof(header.pipelineCacheUUID)) == 0);
}

template <>
std::tuple<SwapchainInfo<GraphicsBackend::Vulkan>, int, PhysicalDeviceProperties<GraphicsBackend::Vulkan>>
getSuitableSwapchainAndQueueFamilyIndex(
	SurfaceHandle<GraphicsBackend::Vulkan> surface, PhysicalDeviceHandle<GraphicsBackend::Vulkan> device)
{
	SwapchainInfo<GraphicsBackend::Vulkan> swapchainInfo;

	VkPhysicalDeviceProperties deviceProperties;
	vkGetPhysicalDeviceProperties(device, &deviceProperties);

	VkPhysicalDeviceFeatures deviceFeatures;
	vkGetPhysicalDeviceFeatures(device, &deviceFeatures);

	assert(deviceFeatures.inheritedQueries);

	vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface, &swapchainInfo.capabilities);

	uint32_t formatCount;
	vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, nullptr);
	if (formatCount != 0)
	{
		swapchainInfo.formats.resize(formatCount);
		vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount,
											 swapchainInfo.formats.data());
	}

	assert(!swapchainInfo.formats.empty());

	uint32_t presentModeCount;
	vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, nullptr);
	if (presentModeCount != 0)
	{
		swapchainInfo.presentModes.resize(presentModeCount);
		vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount,
												  swapchainInfo.presentModes.data());
	}

	assert(!swapchainInfo.presentModes.empty());

	// if (deviceProperties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
	if (deviceProperties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
	{
		uint32_t queueFamilyCount = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);

		std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
		vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

		for (int i = 0; i < static_cast<int>(queueFamilies.size()); i++)
		{
			const auto& queueFamily = queueFamilies[i];

			VkBool32 presentSupport = false;
			vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &presentSupport);

			if (queueFamily.queueCount > 0 && queueFamily.queueFlags & VK_QUEUE_GRAPHICS_BIT &&
				presentSupport)
			{
				return std::make_tuple(swapchainInfo, i, deviceProperties);
			}
		}
	}

	return std::make_tuple(swapchainInfo, -1, deviceProperties);
}

template <>
PipelineLayoutContext<GraphicsBackend::Vulkan> createPipelineLayoutContext(
	DeviceHandle<GraphicsBackend::Vulkan> device,
	const SerializableShaderReflectionModule<GraphicsBackend::Vulkan>& slangModule)
{
	PipelineLayoutContext<GraphicsBackend::Vulkan> pipelineLayout;

	auto shaderDeleter = [device](ShaderModuleHandle<GraphicsBackend::Vulkan>* module, size_t size) {
		for (size_t i = 0; i < size; i++)
			vkDestroyShaderModule(device, *(module + i), nullptr);
	};
	pipelineLayout.shaders =
		std::unique_ptr<ShaderModuleHandle<GraphicsBackend::Vulkan>[], ArrayDeleter<ShaderModuleHandle<GraphicsBackend::Vulkan>>>(
			new ShaderModuleHandle<GraphicsBackend::Vulkan>[slangModule.shaders.size()],
			{shaderDeleter, slangModule.shaders.size()});

	auto descriptorSetLayoutDeleter =
		[device](DescriptorSetLayoutHandle<GraphicsBackend::Vulkan>* layout, size_t size) {
			for (size_t i = 0; i < size; i++)
				vkDestroyDescriptorSetLayout(device, *(layout + i), nullptr);
		};
	pipelineLayout.descriptorSetLayouts = std::unique_ptr<
		DescriptorSetLayoutHandle<GraphicsBackend::Vulkan>[], ArrayDeleter<DescriptorSetLayoutHandle<GraphicsBackend::Vulkan>>>(
		new DescriptorSetLayoutHandle<GraphicsBackend::Vulkan>[slangModule.bindings.size()],
		{descriptorSetLayoutDeleter, slangModule.bindings.size()});

	for (const auto& shader : slangModule.shaders)
	{
		auto createShaderModule = [](DeviceHandle<GraphicsBackend::Vulkan> device, const ShaderEntry& shader)
		{
			VkShaderModuleCreateInfo info = {};
			info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
			info.codeSize = shader.first.size();
			info.pCode = reinterpret_cast<const uint32_t*>(shader.first.data());

			VkShaderModule vkShaderModule;
			CHECK_VK(vkCreateShaderModule(device, &info, nullptr, &vkShaderModule));

			return vkShaderModule;
		};

		pipelineLayout.shaders.get()[&shader - &slangModule.shaders[0]] =
			createShaderModule(device, shader);
	}

	size_t layoutIt = 0;
	for (auto& [space, layoutBindings] : slangModule.bindings)
	{
		pipelineLayout.descriptorSetLayouts.get()[layoutIt++] =
			createDescriptorSetLayout(device, layoutBindings.data(), layoutBindings.size());
	}

	VkPipelineLayoutCreateInfo pipelineLayoutInfo = {};
	pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipelineLayoutInfo.setLayoutCount = slangModule.bindings.size();
	pipelineLayoutInfo.pSetLayouts = pipelineLayout.descriptorSetLayouts.get();
	pipelineLayoutInfo.pushConstantRangeCount = 0;
	pipelineLayoutInfo.pPushConstantRanges = nullptr;

	CHECK_VK(vkCreatePipelineLayout(
		device, &pipelineLayoutInfo, nullptr, &pipelineLayout.layout));

	return pipelineLayout;
}
