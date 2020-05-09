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
bool
isCacheValid<GraphicsBackend::Vulkan>(
	const PipelineCacheHeader<GraphicsBackend::Vulkan>& header,
	const PhysicalDeviceProperties<GraphicsBackend::Vulkan>& physicalDeviceProperties)
{
	return (header.headerLength > 0 &&
		header.cacheHeaderVersion == VK_PIPELINE_CACHE_HEADER_VERSION_ONE &&
		header.vendorID == physicalDeviceProperties.vendorID &&
		header.deviceID == physicalDeviceProperties.deviceID &&
		memcmp(header.pipelineCacheUUID, physicalDeviceProperties.pipelineCacheUUID, sizeof(header.pipelineCacheUUID)) == 0);
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
	PhysicalDeviceHandle<GraphicsBackend::Vulkan> device)
{
    PhysicalDeviceInfo<GraphicsBackend::Vulkan> deviceInfo = {};
	
	vkGetPhysicalDeviceProperties(device, &deviceInfo.deviceProperties);
	vkGetPhysicalDeviceFeatures(device, &deviceInfo.deviceFeatures);
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
		auto createShaderModule = [](DeviceHandle<GraphicsBackend::Vulkan> device, const ShaderEntry& shader)
		{
			VkShaderModuleCreateInfo info = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
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

	VkPipelineLayoutCreateInfo pipelineLayoutInfo = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	pipelineLayoutInfo.setLayoutCount = slangModule.bindings.size();
	pipelineLayoutInfo.pSetLayouts = pipelineLayout.descriptorSetLayouts.get();
	pipelineLayoutInfo.pushConstantRangeCount = 0;
	pipelineLayoutInfo.pPushConstantRanges = nullptr;

	CHECK_VK(vkCreatePipelineLayout(
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

	CHECK_VK(vkCreatePipelineCache(device, &createInfo, nullptr, &cache));

	return cache;
};

template <>
std::vector<std::byte> getPipelineCacheData<GraphicsBackend::Vulkan>(
	DeviceHandle<GraphicsBackend::Vulkan> device,
	PipelineCacheHandle<GraphicsBackend::Vulkan> pipelineCache)
{
	std::vector<std::byte> cacheData;
	size_t cacheDataSize = 0;
	CHECK_VK(vkGetPipelineCacheData(device, pipelineCache, &cacheDataSize, nullptr));
	if (cacheDataSize)
	{
		cacheData.resize(cacheDataSize);
		CHECK_VK(vkGetPipelineCacheData(device, pipelineCache, &cacheDataSize, cacheData.data()));
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
    CHECK_VK(vkCreateDescriptorPool(device, &poolInfo, nullptr, &outDescriptorPool));

    return outDescriptorPool;
}

template <>
PipelineHandle<GraphicsBackend::Vulkan>
createGraphicsPipeline<GraphicsBackend::Vulkan>(
    DeviceHandle<GraphicsBackend::Vulkan> device,
    PipelineCacheHandle<GraphicsBackend::Vulkan> pipelineCache,
    const PipelineConfiguration<GraphicsBackend::Vulkan>& pipelineConfig)
{
    VkPipelineShaderStageCreateInfo vsStageInfo = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    vsStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vsStageInfo.module = pipelineConfig.layout->shaders[0];
    vsStageInfo.pName = "main"; // todo: get from named VkShaderModule object

    // struct AlphaTestSpecializationData
    // {
    // 	uint32_t alphaTestMethod = 0;
    // 	float alphaTestRef = 0.5f;
    // } alphaTestSpecializationData;

    // std::array<VkSpecializationMapEntry, 2> alphaTestSpecializationMapEntries;
    // alphaTestSpecializationMapEntries[0].constantID = 0;
    // alphaTestSpecializationMapEntries[0].size =
    // sizeof(alphaTestSpecializationData.alphaTestMethod);
    // alphaTestSpecializationMapEntries[0].offset = 0;
    // alphaTestSpecializationMapEntries[1].constantID = 1;
    // alphaTestSpecializationMapEntries[1].size =
    // sizeof(alphaTestSpecializationData.alphaTestRef);
    // alphaTestSpecializationMapEntries[1].offset =
    // offsetof(AlphaTestSpecializationData, alphaTestRef);

    // VkSpecializationInfo specializationInfo = {};
    // specializationInfo.dataSize = sizeof(alphaTestSpecializationData);
    // specializationInfo.mapEntryCount =
    // static_cast<uint32_t>(alphaTestSpecializationMapEntries.size());
    // specializationInfo.pMapEntries = alphaTestSpecializationMapEntries.data();
    // specializationInfo.pData = &alphaTestSpecializationData;

    VkPipelineShaderStageCreateInfo fsStageInfo = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    fsStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fsStageInfo.module = pipelineConfig.layout->shaders[1];
    fsStageInfo.pName = "main";
    fsStageInfo.pSpecializationInfo = nullptr; //&specializationInfo;

    VkPipelineShaderStageCreateInfo shaderStages[] = {vsStageInfo, fsStageInfo};

    VkPipelineVertexInputStateCreateInfo vertexInputInfo = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vertexInputInfo.vertexBindingDescriptionCount = pipelineConfig.resources->model->getBindings().size();
    vertexInputInfo.pVertexBindingDescriptions = pipelineConfig.resources->model->getBindings().data();
    vertexInputInfo.vertexAttributeDescriptionCount =
        static_cast<uint32_t>(pipelineConfig.resources->model->getDesc().attributes.size());
    vertexInputInfo.pVertexAttributeDescriptions = pipelineConfig.resources->model->getDesc().attributes.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    const auto& imageExtent = pipelineConfig.resources->renderTarget->imageExtent;

    VkViewport viewport = {};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(imageExtent.width);
    viewport.height = static_cast<float>(imageExtent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    VkRect2D scissor = {};
    scissor.offset = {0, 0};
    scissor.extent = imageExtent;

    VkPipelineViewportStateCreateInfo viewportState = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    viewportState.viewportCount = 1;
    viewportState.pViewports = &viewport;
    viewportState.scissorCount = 1;
    viewportState.pScissors = &scissor;

    VkPipelineRasterizationStateCreateInfo rasterizer = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;
    rasterizer.depthBiasConstantFactor = 0.0f;
    rasterizer.depthBiasClamp = 0.0f;
    rasterizer.depthBiasSlopeFactor = 0.0f;

    VkPipelineMultisampleStateCreateInfo multisampling = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    multisampling.minSampleShading = 1.0f;
    multisampling.pSampleMask = nullptr;
    multisampling.alphaToCoverageEnable = VK_FALSE;
    multisampling.alphaToOneEnable = VK_FALSE;

    VkPipelineDepthStencilStateCreateInfo depthStencil = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.minDepthBounds = 0.0f;
    depthStencil.maxDepthBounds = 1.0f;
    depthStencil.stencilTestEnable = VK_FALSE;
    depthStencil.front = {};
    depthStencil.back = {};

    VkPipelineColorBlendAttachmentState colorBlendAttachment = {};
    colorBlendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
        VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE;
    colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
    colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

    VkPipelineColorBlendStateCreateInfo colorBlending = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.logicOp = VK_LOGIC_OP_COPY;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;
    colorBlending.blendConstants[0] = 0.0f;
    colorBlending.blendConstants[1] = 0.0f;
    colorBlending.blendConstants[2] = 0.0f;
    colorBlending.blendConstants[3] = 0.0f;

    std::array<VkDynamicState, 2> dynamicStates = {VK_DYNAMIC_STATE_VIEWPORT,
                                                    VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    VkGraphicsPipelineCreateInfo pipelineInfo = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pipelineInfo.stageCount = sizeof_array(shaderStages);
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = pipelineConfig.layout->layout;
    pipelineInfo.renderPass = pipelineConfig.resources->renderTarget->renderPass;
    pipelineInfo.subpass = 0;
    pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
    pipelineInfo.basePipelineIndex = -1;

    PipelineHandle<GraphicsBackend::Vulkan> outPipeline = 0;
    CHECK_VK(vkCreateGraphicsPipelines(
        device, pipelineCache, 1, &pipelineInfo, nullptr, &outPipeline));

    return outPipeline;
}
