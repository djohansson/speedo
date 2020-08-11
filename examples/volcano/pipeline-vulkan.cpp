#include "pipeline.h"
#include "vk-utils.h"

#include <core/slang-secure-crt.h>

#pragma pack(push, 1)
template <>
struct PipelineCacheHeader<Vk>
{
	uint32_t headerLength = 0;
	uint32_t cacheHeaderVersion = 0;
	uint32_t vendorID = 0;
	uint32_t deviceID = 0;
	uint8_t pipelineCacheUUID[VK_UUID_SIZE] = {};
};
#pragma pack(pop)

namespace pipeline
{

bool isCacheValid(
	const PipelineCacheHeader<Vk>& header,
	const PhysicalDeviceProperties<Vk>& physicalDeviceProperties)
{
	return (header.headerLength > 0 &&
		header.cacheHeaderVersion == VK_PIPELINE_CACHE_HEADER_VERSION_ONE &&
		header.vendorID == physicalDeviceProperties.properties.vendorID &&
		header.deviceID == physicalDeviceProperties.properties.deviceID &&
		memcmp(header.pipelineCacheUUID, physicalDeviceProperties.properties.pipelineCacheUUID, sizeof(header.pipelineCacheUUID)) == 0);
}

PipelineCacheHandle<Vk> createPipelineCache(
	DeviceHandle<Vk> device,
	const std::vector<std::byte>& cacheData)
{
	PipelineCacheHandle<Vk> cache;

	VkPipelineCacheCreateInfo createInfo = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
	createInfo.initialDataSize = cacheData.size();
	createInfo.pInitialData = cacheData.size() ? cacheData.data() : nullptr;

	VK_CHECK(vkCreatePipelineCache(device, &createInfo, nullptr, &cache));

	return cache;
};


PipelineCacheHandle<Vk> loadPipelineCache(
	const std::filesystem::path& cacheFilePath,
	DeviceHandle<Vk> device,
	PhysicalDeviceProperties<Vk> physicalDeviceProperties)
{
	std::vector<std::byte> cacheData;

	auto loadCacheOp = [&physicalDeviceProperties, &cacheData](std::istream& stream)
	{
		cereal::BinaryInputArchive bin(stream);
		bin(cacheData);

		auto header = reinterpret_cast<const PipelineCacheHeader<Vk>*>(cacheData.data());
		
		if (cacheData.empty() || !isCacheValid(*header, physicalDeviceProperties))
		{
			std::cout << "Invalid pipeline cache, creating new." << std::endl;
			cacheData.clear();
			return;
		}
	};

	auto [sourceFileState, sourceFileInfo] = getFileInfo(cacheFilePath, false);
	if (sourceFileState != FileState::Missing)
		auto [sourceFileState, sourceFileInfo] = loadBinaryFile(cacheFilePath, loadCacheOp, false);

	return createPipelineCache(device, cacheData);
}

std::vector<std::byte> getPipelineCacheData(
	DeviceHandle<Vk> device,
	PipelineCacheHandle<Vk> pipelineCache)
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

std::tuple<FileState, FileInfo> savePipelineCache(
	const std::filesystem::path& cacheFilePath,
	DeviceHandle<Vk> device,
	PhysicalDeviceProperties<Vk> physicalDeviceProperties,
	PipelineCacheHandle<Vk> pipelineCache)
{
	// todo: move to gfx-vulkan.cpp
	auto saveCacheOp = [&device, &pipelineCache, &physicalDeviceProperties](std::ostream& stream)
	{
		auto cacheData = getPipelineCacheData(device, pipelineCache);
		if (!cacheData.empty())
		{
			auto header = reinterpret_cast<const PipelineCacheHeader<Vk>*>(cacheData.data());

			if (cacheData.empty() || !isCacheValid(*header, physicalDeviceProperties))
			{
				std::cout << "Invalid pipeline cache, something is seriously wrong. Exiting." << std::endl;
				return;
			}
			
			cereal::BinaryOutputArchive bin(stream);
			bin(cacheData);
		}
		else
			assertf(false, "Failed to get pipeline cache.");
	};

	return saveBinaryFile(cacheFilePath, saveCacheOp, false);
}

}

template <>
PipelineLayout<Vk>::PipelineLayout(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    DescriptorSetLayoutVector<Vk>&& descriptorSetLayouts,
    PipelineLayoutHandle<Vk>&& layout)
: DeviceResource<Vk>(
    deviceContext,
    {"_PipelineLayout"},
    1,
    VK_OBJECT_TYPE_PIPELINE_LAYOUT,
    reinterpret_cast<uint64_t*>(&layout))
, myDescriptorSetLayouts(std::move(descriptorSetLayouts))
, myLayout(std::move(layout))
{
}

template <>
PipelineLayout<Vk>::PipelineLayout(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    DescriptorSetLayoutVector<Vk>&& descriptorSetLayouts)
: PipelineLayout(
    deviceContext,
    std::move(descriptorSetLayouts),
    createPipelineLayout(
        deviceContext->getDevice(),
        descriptorSetLayouts.data(),
        descriptorSetLayouts.size()))
{
}

template <>
PipelineLayout<Vk>::PipelineLayout(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    const std::shared_ptr<SerializableShaderReflectionModule<Vk>>& slangModule)
: PipelineLayout(
    deviceContext,
    DescriptorSetLayoutVector<Vk>(
        deviceContext,
        slangModule->bindings))
{
    auto device = deviceContext->getDevice();

	auto shaderDeleter = [device](ShaderModuleHandle<Vk>* module, size_t size) {
		for (size_t i = 0; i < size; i++)
			vkDestroyShaderModule(device, *(module + i), nullptr);
	};

	myShaders = std::unique_ptr<ShaderModuleHandle<Vk>[], ArrayDeleter<ShaderModuleHandle<Vk>>>(
			new ShaderModuleHandle<Vk>[slangModule->shaders.size()],
			{shaderDeleter, slangModule->shaders.size()});

    char stringBuffer[128];

	for (const auto& shader : slangModule->shaders)
	{
        uint32_t shaderIt = &shader - &slangModule->shaders[0];
		myShaders.get()[shaderIt] =
            createShaderModule(
                device,
                shader.first.size(),
                reinterpret_cast<const uint32_t *>(shader.first.data()));
    
        static constexpr std::string_view shaderModuleStr = "_ShaderModule";

        sprintf_s(
            stringBuffer,
            sizeof(stringBuffer),
            "%.*s%.*s%u",
            getName().size(),
            getName().c_str(),
            static_cast<int>(shaderModuleStr.size()),
            shaderModuleStr.data(),
            shaderIt);

        getDeviceContext()->addOwnedObject(
            getId(),
            VK_OBJECT_TYPE_SHADER_MODULE,
            reinterpret_cast<uint64_t>(myShaders.get()[shaderIt]),
            stringBuffer);
    }
}

template <>
PipelineLayout<Vk>::~PipelineLayout()
{
    if (myLayout)
        vkDestroyPipelineLayout(getDeviceContext()->getDevice(), myLayout, nullptr);
}

template<>
uint64_t PipelineContext<Vk>::internalCalculateHashKey() const
{
    // todo
    return 0;
}

template <>
PipelineHandle<Vk> PipelineContext<Vk>::internalCreateGraphicsPipeline(uint64_t hashKey)
{
    VkPipelineShaderStageCreateInfo vsStageInfo = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    vsStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vsStageInfo.module = myLayout->getShaders()[0];
    vsStageInfo.pName = "vertexMain"; // todo: get from named VkShaderModule object

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
    fsStageInfo.module = myLayout->getShaders()[1];
    fsStageInfo.pName = "fragmentMain"; // todo: get from named VkShaderModule object
    fsStageInfo.pSpecializationInfo = nullptr; //&specializationInfo;

    VkPipelineShaderStageCreateInfo shaderStages[] = {vsStageInfo, fsStageInfo};

    VkPipelineVertexInputStateCreateInfo vertexInputInfo = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vertexInputInfo.vertexBindingDescriptionCount = myResources->model->getBindings().size();
    vertexInputInfo.pVertexBindingDescriptions = myResources->model->getBindings().data();
    vertexInputInfo.vertexAttributeDescriptionCount =
        static_cast<uint32_t>(myResources->model->getDesc().attributes.size());
    vertexInputInfo.pVertexAttributeDescriptions = myResources->model->getDesc().attributes.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    const auto& extent = myResources->renderTarget->getRenderTargetDesc().extent;

    VkViewport viewport = {};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    VkRect2D scissor = {};
    scissor.offset = {0, 0};
    scissor.extent = {extent.width, extent.height};

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
    pipelineInfo.layout = myLayout->getLayout();
    pipelineInfo.renderPass = std::get<0>(myResources->renderTarget->renderPassAndFramebuffer());
    pipelineInfo.subpass = myResources->renderTarget->getSubpass().value_or(0);
    pipelineInfo.basePipelineHandle = 0;
    pipelineInfo.basePipelineIndex = -1;

    VkPipeline pipelineHandle;
    VK_CHECK(vkCreateGraphicsPipelines(
        getDeviceContext()->getDevice(),
        myCache,
        1,
        &pipelineInfo,
        nullptr,
        &pipelineHandle));
    
    char stringBuffer[128];

    static constexpr std::string_view pipelineStr = "_Pipeline";
    
    sprintf_s(
        stringBuffer,
        sizeof(stringBuffer),
        "%.*s%.*s%u",
        getName().size(),
        getName().c_str(),
        static_cast<int>(pipelineStr.size()),
        pipelineStr.data(),
        hashKey);

    getDeviceContext()->addOwnedObject(
        getId(),
        VK_OBJECT_TYPE_PIPELINE,
        reinterpret_cast<uint64_t>(pipelineHandle),
        stringBuffer);

    return pipelineHandle;
}

template <>
PipelineContext<Vk>::PipelineMap::const_iterator PipelineContext<Vk>::internalUpdateMap()
{
    auto hashKey = internalCalculateHashKey();
    auto emplaceResult = myPipelineMap.emplace(
        hashKey,
        PipelineHandle<Vk>{});

    if (emplaceResult.second)
        emplaceResult.first->second = internalCreateGraphicsPipeline(hashKey);

    return emplaceResult.first;
}

template <>
PipelineHandle<Vk> PipelineContext<Vk>::getPipeline()
{
    std::unique_lock writeLock(myMutex);
    
    return internalUpdateMap()->second;
}

template <>
void PipelineContext<Vk>::updateDescriptorSets(BufferHandle<Vk> buffer)
{
    ZoneScopedN("PipelineContext::updateDescriptorSets");

    if (!myDescriptorSets)
        myDescriptorSets = std::make_shared<DescriptorSetVector<Vk>>(
            getDeviceContext(),
            myLayout->getDescriptorSetLayouts());

    // todo: use reflection
    
    VkDescriptorBufferInfo bufferInfo = {};
    bufferInfo.buffer = buffer;
    bufferInfo.offset = 0;
    bufferInfo.range = VK_WHOLE_SIZE;

    VkDescriptorImageInfo imageInfo = {};
    imageInfo.imageLayout = myResources->image->getImageLayout();
    imageInfo.imageView = myResources->imageView->getImageViewHandle();
    imageInfo.sampler = myResources->sampler;

    std::array<VkWriteDescriptorSet, 3> descriptorWrites = {};
    descriptorWrites[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    descriptorWrites[0].dstSet = myDescriptorSets->data()[0];
    descriptorWrites[0].dstBinding = 0;
    descriptorWrites[0].dstArrayElement = 0;
    descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    descriptorWrites[0].descriptorCount = 1;
    descriptorWrites[0].pBufferInfo = &bufferInfo;
    descriptorWrites[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    descriptorWrites[1].dstSet = myDescriptorSets->data()[0];
    descriptorWrites[1].dstBinding = 1;
    descriptorWrites[1].dstArrayElement = 0;
    descriptorWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    descriptorWrites[1].descriptorCount = 1;
    descriptorWrites[1].pImageInfo = &imageInfo;
    descriptorWrites[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    descriptorWrites[2].dstSet = myDescriptorSets->data()[0];
    descriptorWrites[2].dstBinding = 2;
    descriptorWrites[2].dstArrayElement = 0;
    descriptorWrites[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    descriptorWrites[2].descriptorCount = 1;
    descriptorWrites[2].pImageInfo = &imageInfo;

    vkUpdateDescriptorSets(
        getDeviceContext()->getDevice(),
        static_cast<uint32_t>(descriptorWrites.size()),
        descriptorWrites.data(),
        0,
        nullptr);
}

template<>
PipelineContext<Vk>::PipelineContext(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    PipelineContextCreateDesc<Vk>&& desc)
: DeviceResource(deviceContext, desc)
, myDesc(std::move(desc))
{
    char stringBuffer[128];

    myResources = std::make_shared<PipelineResourceView<Vk>>();
    myResources->sampler = createSampler(deviceContext->getDevice());

    static constexpr std::string_view samplerStr = "_Sampler";

    sprintf_s(
        stringBuffer,
        sizeof(stringBuffer),
        "%.*s%.*s",
        getName().size(),
        getName().c_str(),
        static_cast<int>(samplerStr.size()),
        samplerStr.data());

    deviceContext->addOwnedObject(
        getId(),
        VK_OBJECT_TYPE_SAMPLER,
        reinterpret_cast<uint64_t>(myResources->sampler),
        stringBuffer);

    myCache = pipeline::loadPipelineCache(
        myDesc.cachePath,
        deviceContext->getDevice(),
        deviceContext->getPhysicalDeviceInfo().deviceProperties);
    
    static constexpr std::string_view pipelineCacheStr = "_PipelineCache";

    sprintf_s(
        stringBuffer,
        sizeof(stringBuffer),
        "%.*s%.*s",
        getName().size(),
        getName().c_str(),
        static_cast<int>(pipelineCacheStr.size()),
        pipelineCacheStr.data());

    deviceContext->addOwnedObject(
        getId(),
        VK_OBJECT_TYPE_PIPELINE_CACHE,
        reinterpret_cast<uint64_t>(myCache),
        stringBuffer);
}

template<>
PipelineContext<Vk>::~PipelineContext()
{
    pipeline::savePipelineCache(
        myDesc.cachePath,
        getDeviceContext()->getDevice(),
        getDeviceContext()->getPhysicalDeviceInfo().deviceProperties,
        myCache);

    for (const auto& pipelineIt : myPipelineMap)
        vkDestroyPipeline(getDeviceContext()->getDevice(), pipelineIt.second, nullptr);
    
    vkDestroyPipelineCache(getDeviceContext()->getDevice(), myCache, nullptr);
    vkDestroySampler(getDeviceContext()->getDevice(), myResources->sampler, nullptr);
}