#include "gfx.h"
#include "vk-utils.h"

#include <algorithm>
#include <tuple>
#include <vector>

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
bool isCacheValid<GraphicsBackend::Vulkan>(
	const PipelineCacheHeader<GraphicsBackend::Vulkan>& header,
	const PhysicalDeviceProperties<GraphicsBackend::Vulkan>& physicalDeviceProperties)
{
	return (header.headerLength > 0 &&
		header.cacheHeaderVersion == VK_PIPELINE_CACHE_HEADER_VERSION_ONE &&
		header.vendorID == physicalDeviceProperties.properties.vendorID &&
		header.deviceID == physicalDeviceProperties.properties.deviceID &&
		memcmp(header.pipelineCacheUUID, physicalDeviceProperties.properties.pipelineCacheUUID, sizeof(header.pipelineCacheUUID)) == 0);
}

template <>
SurfaceCapabilities<GraphicsBackend::Vulkan> getSurfaceCapabilities<GraphicsBackend::Vulkan>(
    SurfaceHandle<GraphicsBackend::Vulkan> surface, PhysicalDeviceHandle<GraphicsBackend::Vulkan> device)
{
    SurfaceCapabilities<GraphicsBackend::Vulkan> capabilities;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface, &capabilities);

    return capabilities;
}

template <>
PhysicalDeviceInfo<GraphicsBackend::Vulkan> getPhysicalDeviceInfo<GraphicsBackend::Vulkan>(
	SurfaceHandle<GraphicsBackend::Vulkan> surface,
    InstanceHandle<GraphicsBackend::Vulkan> instance,
	PhysicalDeviceHandle<GraphicsBackend::Vulkan> device)
{
    PhysicalDeviceInfo<GraphicsBackend::Vulkan> deviceInfo = {};
    VkPhysicalDeviceTimelineSemaphoreProperties timelineProperties = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_PROPERTIES };
    deviceInfo.deviceProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    deviceInfo.deviceProperties.pNext = &timelineProperties;
    deviceInfo.deviceFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    deviceInfo.deviceFeatures.pNext = nullptr;
    
    auto _vkGetPhysicalDeviceProperties2 =
        (PFN_vkGetPhysicalDeviceProperties2)vkGetInstanceProcAddr(
            instance, "vkGetPhysicalDeviceProperties2");
    assert(_vkGetPhysicalDeviceProperties2 != nullptr);
    _vkGetPhysicalDeviceProperties2(device, &deviceInfo.deviceProperties);
	
    auto _vkGetPhysicalDeviceFeatures2 =
        (PFN_vkGetPhysicalDeviceFeatures2)vkGetInstanceProcAddr(
            instance, "vkGetPhysicalDeviceFeatures2");
    assert(_vkGetPhysicalDeviceFeatures2 != nullptr);
    _vkGetPhysicalDeviceFeatures2(device, &deviceInfo.deviceFeatures);
    
	deviceInfo.swapchainInfo.capabilities = getSurfaceCapabilities<GraphicsBackend::Vulkan>(surface, device);

	uint32_t formatCount;
	vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, nullptr);
	if (formatCount != 0)
	{
		deviceInfo.swapchainInfo.formats.resize(formatCount);
		vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount,
											 deviceInfo.swapchainInfo.formats.data());
	}

	uint32_t presentModeCount;
	vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, nullptr);
	if (presentModeCount != 0)
	{
		deviceInfo.swapchainInfo.presentModes.resize(presentModeCount);
		vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount,
												  deviceInfo.swapchainInfo.presentModes.data());
	}

    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
    if (queueFamilyCount != 0)
	{
        deviceInfo.queueFamilyProperties.resize(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, deviceInfo.queueFamilyProperties.data());

        deviceInfo.queueFamilyPresentSupport.resize(queueFamilyCount);
        for (uint32_t queueFamilyIt = 0; queueFamilyIt < queueFamilyCount; queueFamilyIt++)
            vkGetPhysicalDeviceSurfaceSupportKHR(device, queueFamilyIt, surface, &deviceInfo.queueFamilyPresentSupport[queueFamilyIt]);
    }
    
	return deviceInfo;
}

template <>
void createLayoutBindings<GraphicsBackend::Vulkan>(
    slang::VariableLayoutReflection* parameter,
    typename SerializableShaderReflectionModule<GraphicsBackend::Vulkan>::BindingsMap& bindings)
{
	slang::TypeLayoutReflection* typeLayout = parameter->getTypeLayout();

	std::cout << "name: " << parameter->getName()
				<< ", index: " << parameter->getBindingIndex()
				<< ", space: " << parameter->getBindingSpace()
				<< ", stage: " << parameter->getStage()
				<< ", kind: " << (int)typeLayout->getKind()
				<< ", typeName: " << typeLayout->getName();

	unsigned categoryCount = parameter->getCategoryCount();
	for (unsigned cc = 0; cc < categoryCount; cc++)
	{
		slang::ParameterCategory category = parameter->getCategoryByIndex(cc);

		size_t offsetForCategory = parameter->getOffset(category);
		size_t spaceForCategory = parameter->getBindingSpace(category);

		std::cout << ", category: " << category
					<< ", offsetForCategory: " << offsetForCategory
					<< ", spaceForCategory: " << spaceForCategory;

		if (category == slang::ParameterCategory::DescriptorTableSlot)
		{
			using MapType = std::remove_reference_t<decltype(bindings)>;
			using VectorType = typename MapType::mapped_type;
			using BindingType = typename VectorType::value_type;

			BindingType binding;
			binding.binding = parameter->getBindingIndex();
			binding.descriptorCount = typeLayout->isArray() ? typeLayout->getElementCount() : 1;
			binding.stageFlags = VK_SHADER_STAGE_ALL;
			binding.pImmutableSamplers = nullptr; // todo - initialize these
            //binding.immutableSamplerCreateInfos.push_back(...)

			switch (parameter->getStage())
			{
			case SLANG_STAGE_VERTEX:
				binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
				break;
			case SLANG_STAGE_FRAGMENT:
				binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
				break;
			case SLANG_STAGE_HULL:
				binding.stageFlags = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
				break;
			case SLANG_STAGE_DOMAIN:
				binding.stageFlags = VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
				break;
			case SLANG_STAGE_GEOMETRY:
				binding.stageFlags = VK_SHADER_STAGE_GEOMETRY_BIT;
				break;
			case SLANG_STAGE_COMPUTE:
				binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
				break;
			case SLANG_STAGE_RAY_GENERATION:
				binding.stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
				break;
			case SLANG_STAGE_INTERSECTION:
				binding.stageFlags = VK_SHADER_STAGE_INTERSECTION_BIT_KHR;
				break;
			case SLANG_STAGE_ANY_HIT:
				binding.stageFlags = VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
				break;
			case SLANG_STAGE_CLOSEST_HIT:
				binding.stageFlags = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
				break;
			case SLANG_STAGE_MISS:
				binding.stageFlags = VK_SHADER_STAGE_MISS_BIT_KHR;
				break;
			case SLANG_STAGE_CALLABLE:
				binding.stageFlags = VK_SHADER_STAGE_CALLABLE_BIT_KHR;
				break;
			case SLANG_STAGE_NONE:
				break;
			default:
				assert(false); // please implement me!
				break;
			}

			switch (typeLayout->getKind())
			{
			case slang::TypeReflection::Kind::ConstantBuffer:
				binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
				break;
			case slang::TypeReflection::Kind::Resource:
				binding.descriptorType =
					VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; // "resource" might be more
														// generic tho...
				break;
			case slang::TypeReflection::Kind::SamplerState:
				binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
				break;
			case slang::TypeReflection::Kind::None:
			case slang::TypeReflection::Kind::Struct:
			case slang::TypeReflection::Kind::Array:
			case slang::TypeReflection::Kind::Matrix:
			case slang::TypeReflection::Kind::Vector:
			case slang::TypeReflection::Kind::Scalar:
			case slang::TypeReflection::Kind::TextureBuffer:
			case slang::TypeReflection::Kind::ShaderStorageBuffer:
			case slang::TypeReflection::Kind::ParameterBlock:
			case slang::TypeReflection::Kind::GenericTypeParameter:
			case slang::TypeReflection::Kind::Interface:
			case slang::TypeReflection::Kind::OutputStream:
			case slang::TypeReflection::Kind::Specialized:
			default:
				assert(false); // please implement me!
				break;
			}

			bindings[parameter->getBindingSpace()].push_back(binding);
		}
	}

	std::cout << std::endl;

	// unsigned fieldCount = typeLayout->getFieldCount();
	// for (unsigned ff = 0; ff < fieldCount; ff++)
	// 	createLayoutBindings<GraphicsBackend::Vulkan>(typeLayout->getFieldByIndex(ff), bindings);
}

template <>
PipelineLayoutContext<GraphicsBackend::Vulkan>
createPipelineLayoutContext<GraphicsBackend::Vulkan>(
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
		pipelineLayout.shaders.get()[&shader - &slangModule.shaders[0]] =
            createShaderModule(device, shader.first.size(), reinterpret_cast<const uint32_t *>(shader.first.data()));
	}

	size_t layoutIt = 0;
	for (auto& [space, layoutBindings] : slangModule.bindings)
	{
		pipelineLayout.descriptorSetLayouts.get()[layoutIt++] =
			createDescriptorSetLayout(device, layoutBindings.data(), layoutBindings.size());
	}

	VkPipelineLayoutCreateInfo pipelineLayoutInfo = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	pipelineLayoutInfo.setLayoutCount = slangModule.bindings.size();
	pipelineLayoutInfo.pSetLayouts = pipelineLayout.descriptorSetLayouts.get();
	pipelineLayoutInfo.pushConstantRangeCount = 0;
	pipelineLayoutInfo.pPushConstantRanges = nullptr;

	VK_CHECK(vkCreatePipelineLayout(
		device, &pipelineLayoutInfo, nullptr, &pipelineLayout.layout));

	return pipelineLayout;
}

template <>
PipelineCacheHandle<GraphicsBackend::Vulkan>
createPipelineCache<GraphicsBackend::Vulkan>(
	DeviceHandle<GraphicsBackend::Vulkan> device,
	const std::vector<std::byte>& cacheData)
{
	PipelineCacheHandle<GraphicsBackend::Vulkan> cache;

	VkPipelineCacheCreateInfo createInfo = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
	createInfo.initialDataSize = cacheData.size();
	createInfo.pInitialData = cacheData.size() ? cacheData.data() : nullptr;

	VK_CHECK(vkCreatePipelineCache(device, &createInfo, nullptr, &cache));

	return cache;
};

template <>
std::vector<std::byte> getPipelineCacheData<GraphicsBackend::Vulkan>(
	DeviceHandle<GraphicsBackend::Vulkan> device,
	PipelineCacheHandle<GraphicsBackend::Vulkan> pipelineCache)
{
	std::vector<std::byte> cacheData;
	size_t cacheDataSize = 0;
	VK_CHECK(vkGetPipelineCacheData(device, pipelineCache, &cacheDataSize, nullptr));
	if (cacheDataSize)
	{
		cacheData.resize(cacheDataSize);
		VK_CHECK(vkGetPipelineCacheData(device, pipelineCache, &cacheDataSize, cacheData.data()));
	}

	return cacheData;
};

template <>
AllocatorHandle<GraphicsBackend::Vulkan>
createAllocator<GraphicsBackend::Vulkan>(
	InstanceHandle<GraphicsBackend::Vulkan> instance,
    DeviceHandle<GraphicsBackend::Vulkan> device,
    PhysicalDeviceHandle<GraphicsBackend::Vulkan> physicalDevice)
{
    auto vkGetBufferMemoryRequirements2KHR =
        (PFN_vkGetBufferMemoryRequirements2KHR)vkGetInstanceProcAddr(
            instance, "vkGetBufferMemoryRequirements2KHR");
    assert(vkGetBufferMemoryRequirements2KHR != nullptr);

    auto vkGetImageMemoryRequirements2KHR =
        (PFN_vkGetImageMemoryRequirements2KHR)vkGetInstanceProcAddr(
            instance, "vkGetImageMemoryRequirements2KHR");
    assert(vkGetImageMemoryRequirements2KHR != nullptr);

    VmaVulkanFunctions functions = {};
    functions.vkGetPhysicalDeviceProperties = vkGetPhysicalDeviceProperties;
    functions.vkGetPhysicalDeviceMemoryProperties = vkGetPhysicalDeviceMemoryProperties;
    functions.vkAllocateMemory = vkAllocateMemory;
    functions.vkFreeMemory = vkFreeMemory;
    functions.vkMapMemory = vkMapMemory;
    functions.vkUnmapMemory = vkUnmapMemory;
    functions.vkFlushMappedMemoryRanges = vkFlushMappedMemoryRanges;
    functions.vkInvalidateMappedMemoryRanges = vkInvalidateMappedMemoryRanges;
    functions.vkBindBufferMemory = vkBindBufferMemory;
    functions.vkBindImageMemory = vkBindImageMemory;
    functions.vkGetBufferMemoryRequirements = vkGetBufferMemoryRequirements;
    functions.vkGetImageMemoryRequirements = vkGetImageMemoryRequirements;
    functions.vkCreateBuffer = vkCreateBuffer;
    functions.vkDestroyBuffer = vkDestroyBuffer;
    functions.vkCreateImage = vkCreateImage;
    functions.vkDestroyImage = vkDestroyImage;
    functions.vkGetBufferMemoryRequirements2KHR = vkGetBufferMemoryRequirements2KHR;
    functions.vkGetImageMemoryRequirements2KHR = vkGetImageMemoryRequirements2KHR;

    VmaAllocator allocator;
    VmaAllocatorCreateInfo allocatorInfo = {};
    allocatorInfo.instance = instance;
    allocatorInfo.physicalDevice = physicalDevice;
    allocatorInfo.device = device;
    allocatorInfo.pVulkanFunctions = &functions;
    vmaCreateAllocator(&allocatorInfo, &allocator);

    return allocator;
}

template <>
DescriptorPoolHandle<GraphicsBackend::Vulkan>
createDescriptorPool<GraphicsBackend::Vulkan>(DeviceHandle<GraphicsBackend::Vulkan> device)
{
    constexpr uint32_t maxDescriptorCount = 1000;
    VkDescriptorPoolSize poolSizes[] = {
        {VK_DESCRIPTOR_TYPE_SAMPLER, maxDescriptorCount},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, maxDescriptorCount},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, maxDescriptorCount},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, maxDescriptorCount},
        {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, maxDescriptorCount},
        {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, maxDescriptorCount},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, maxDescriptorCount},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, maxDescriptorCount},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, maxDescriptorCount},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, maxDescriptorCount},
        {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, maxDescriptorCount}};

    VkDescriptorPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    poolInfo.poolSizeCount = static_cast<uint32_t>(sizeof_array(poolSizes));
    poolInfo.pPoolSizes = poolSizes;
    poolInfo.maxSets = maxDescriptorCount * static_cast<uint32_t>(sizeof_array(poolSizes));
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;

    DescriptorPoolHandle<GraphicsBackend::Vulkan> outDescriptorPool;
    VK_CHECK(vkCreateDescriptorPool(device, &poolInfo, nullptr, &outDescriptorPool));

    return outDescriptorPool;
}
