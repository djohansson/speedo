#include "pipeline.h"
#include "vk-utils.h"

#include <stb_sprintf.h>

#include <cereal/archives/lz4_binary.hpp>

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
	const std::vector<char>& cacheData)
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
	std::vector<char> cacheData;

	auto loadCacheOp = [&physicalDeviceProperties, &cacheData](std::istream& stream)
	{
		cereal::LZ4BinaryInputArchive bin(stream);
		bin(cacheData);

		auto header = reinterpret_cast<const PipelineCacheHeader<Vk>*>(cacheData.data());
		
		if (cacheData.empty() || !isCacheValid(*header, physicalDeviceProperties))
		{
			std::cout << "Invalid pipeline cache, creating new." << std::endl;
			cacheData.clear();
			return false;
		}

        return true;
	};

	auto [fileState, fileInfo] = getFileInfo(cacheFilePath, false);
	if (fileState != FileState::Missing)
		auto [fileState, fileInfo] = loadBinaryFile(cacheFilePath, loadCacheOp, false);

	return (fileState == FileState::Valid ? createPipelineCache(device, cacheData) : VK_NULL_HANDLE);
}

std::vector<char> getPipelineCacheData(
	DeviceHandle<Vk> device,
	PipelineCacheHandle<Vk> pipelineCache)
{
	std::vector<char> cacheData;
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
		if (auto cacheData = getPipelineCacheData(device, pipelineCache); !cacheData.empty())
		{
			auto header = reinterpret_cast<const PipelineCacheHeader<Vk>*>(cacheData.data());

			if (!isCacheValid(*header, physicalDeviceProperties))
			{
				std::cout << "Invalid pipeline cache, will not save. Exiting." << std::endl;
				return false;
			}
			
			cereal::LZ4BinaryOutputArchive bin(stream);
			bin(cacheData);
		}
		else
        {
            std::cout << "Failed to get pipeline cache. Exiting." << std::endl;
			return false;
        }

        return true;
	};

	return saveBinaryFile(cacheFilePath, saveCacheOp, false);
}

}

template <>
PipelineLayout<Vk>::PipelineLayout(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    std::vector<ShaderModule<Vk>>&& shaderModules,
    std::vector<DescriptorSetLayout<Vk>>&& descriptorSetLayouts,
    PipelineLayoutHandle<Vk>&& layout)
: DeviceResource<Vk>(
    deviceContext,
    {"_PipelineLayout"},
    1,
    VK_OBJECT_TYPE_PIPELINE_LAYOUT,
    reinterpret_cast<uint64_t*>(&layout))
, myShaders(std::move(shaderModules))
, myDescriptorSetLayouts(std::move(descriptorSetLayouts))
, myLayout(std::move(layout))
{
}

template <>
PipelineLayout<Vk>::PipelineLayout(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    std::vector<ShaderModule<Vk>>&& shaderModules,
    std::vector<DescriptorSetLayout<Vk>>&& descriptorSetLayouts)
: PipelineLayout(
    deviceContext,
    std::move(shaderModules),
    std::move(descriptorSetLayouts),
    [device = deviceContext->getDevice(), handles = descriptorset::getDescriptorSetLayoutHandles<Vk>(descriptorSetLayouts)]
    {
        return createPipelineLayout(device, handles.data(), handles.size());
    }())
{
}

template <>
PipelineLayout<Vk>::PipelineLayout(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    const std::shared_ptr<ShaderReflectionInfo<Vk>>& slangModule)
: PipelineLayout(
    deviceContext,
    [&slangModule, &deviceContext]
    {
        std::vector<ShaderModule<Vk>> shaderModules;
        shaderModules.reserve(slangModule->shaders.size());
        for (auto shader : slangModule->shaders)
            shaderModules.emplace_back(ShaderModule<Vk>(deviceContext, shader));
        return shaderModules;
    }(),
    [&slangModule, &deviceContext]
    {
        std::vector<DescriptorSetLayout<Vk>> layouts;
        layouts.reserve(slangModule->bindingsMap.size());
        for (auto setBindings : slangModule->bindingsMap)
            layouts.emplace_back(
                DescriptorSetLayout<Vk>(
                    deviceContext,
                    DescriptorSetLayoutCreateDesc<Vk>{
                        {"DescriptorSetLayout"},
                        std::get<2>(setBindings.second)},
                    std::get<1>(setBindings.second),
                    std::get<0>(setBindings.second)));
        return layouts;
    }())
{
}

template <>
PipelineLayout<Vk>::~PipelineLayout()
{
    if (myLayout)
        vkDestroyPipelineLayout(getDeviceContext()->getDevice(), myLayout, nullptr);
}

template<>
uint64_t Pipeline<Vk>::internalCalculateHashKey() const
{
    // todo
    return 0;
}

template <>
PipelineHandle<Vk> Pipeline<Vk>::internalCreateGraphicsPipeline(uint64_t hashKey)
{
    myShaderStages.clear();
    for (const auto& shader : myLayout->getShaders())
    {
        if (shader.getEntryPoint().second ^ VK_SHADER_STAGE_COMPUTE_BIT)
            myShaderStages.emplace_back(
                PipelineShaderStageCreateInfo<Vk>{
                    VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                    nullptr,
                    0,
                    shader.getEntryPoint().second,
                    shader,
                    shader.getEntryPoint().first.c_str(),
                    nullptr});

    }

    myVertexInput = {
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        nullptr,
        0,
        static_cast<uint32_t>(myResources->model->getBindings().size()),
        myResources->model->getBindings().data(),
        static_cast<uint32_t>(myResources->model->getDesc().attributes.size()),
        myResources->model->getDesc().attributes.data()};

    myInputAssembly = {
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        nullptr,
        0,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        VK_FALSE};

    const auto& extent = myResources->renderTarget->getRenderTargetDesc().extent;

    myViewports.clear();
    myViewports.emplace_back(
        Viewport<Vk>{
            0.0f,
            0.0f,
            static_cast<float>(extent.width),
            static_cast<float>(extent.height),
            0.0f,
            1.0f});

    myScissorRects.clear();
    myScissorRects.emplace_back(Rect2D<Vk>{{0, 0}, {extent.width, extent.height}});

    myViewport = {
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        nullptr,
        0,
        static_cast<uint32_t>(myViewports.size()),
        myViewports.data(),
        static_cast<uint32_t>(myScissorRects.size()),
        myScissorRects.data()};

    myRasterization = {
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        nullptr,
        0,
        VK_FALSE,
        VK_FALSE,
        VK_POLYGON_MODE_FILL,
        VK_CULL_MODE_BACK_BIT,
        VK_FRONT_FACE_COUNTER_CLOCKWISE,
        VK_FALSE,
        0.0f,
        0.0f,
        0.0f,
        1.0f};

    myMultisample = {
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        nullptr,
        0,
        VK_SAMPLE_COUNT_1_BIT,
        VK_FALSE,
        1.0f,
        nullptr,
        VK_FALSE,
        VK_FALSE};

    myDepthStencil = {
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        nullptr,
        0,
        VK_TRUE,
        VK_TRUE,
        VK_COMPARE_OP_LESS,
        VK_FALSE,
        VK_FALSE,
        {},
        {},
        0.0f,
        1.0f};

    myColorBlendAttachments.clear();
    myColorBlendAttachments.emplace_back(
        PipelineColorBlendAttachmentState<Vk>{
            VK_FALSE,
            VK_BLEND_FACTOR_ONE,
            VK_BLEND_FACTOR_ZERO,
            VK_BLEND_OP_ADD,
            VK_BLEND_FACTOR_ONE,
            VK_BLEND_FACTOR_ZERO,
            VK_BLEND_OP_ADD,
            VK_COLOR_COMPONENT_R_BIT |
            VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT |
            VK_COLOR_COMPONENT_A_BIT});
    
    myColorBlend = {
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        nullptr,
        0,
        VK_FALSE,
        VK_LOGIC_OP_COPY,
        static_cast<uint32_t>(myColorBlendAttachments.size()),
        myColorBlendAttachments.data(),
        {0.0f, 0.0f, 0.0f, 0.0f}};

    myDynamicStateDescs.clear();
    myDynamicStateDescs.emplace_back(VK_DYNAMIC_STATE_VIEWPORT);
    myDynamicStateDescs.emplace_back(VK_DYNAMIC_STATE_SCISSOR);
    
    myDynamicState = {
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        nullptr,
        0,
        static_cast<uint32_t>(myDynamicStateDescs.size()),
        myDynamicStateDescs.data()};

    auto renderPassAndFramebuffer = myResources->renderTarget->renderPassAndFramebuffer();

    VkGraphicsPipelineCreateInfo pipelineInfo = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pipelineInfo.stageCount = static_cast<uint32_t>(myShaderStages.size());
    pipelineInfo.pStages = myShaderStages.data();
    pipelineInfo.pVertexInputState = &myVertexInput;
    pipelineInfo.pInputAssemblyState = &myInputAssembly;
    pipelineInfo.pViewportState = &myViewport;
    pipelineInfo.pRasterizationState = &myRasterization;
    pipelineInfo.pMultisampleState = &myMultisample;
    pipelineInfo.pDepthStencilState = &myDepthStencil;
    pipelineInfo.pColorBlendState = &myColorBlend;
    pipelineInfo.pDynamicState = &myDynamicState;
    pipelineInfo.layout = *myLayout;
    pipelineInfo.renderPass = std::get<0>(renderPassAndFramebuffer);
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
    
    stbsp_sprintf(
        stringBuffer,
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
Pipeline<Vk>::PipelineMap::const_iterator Pipeline<Vk>::internalUpdateMap()
{
    auto hashKey = internalCalculateHashKey();
    auto emplaceResult = myPipelineMap.emplace(
        hashKey,
        PipelineHandle<Vk>{});

    if (emplaceResult.second)
        emplaceResult.first->second = internalCreateGraphicsPipeline(hashKey);

    return emplaceResult.first;
}

template<>
Pipeline<Vk>::Pipeline(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    PipelineCreateDesc<Vk>&& desc)
: DeviceResource(deviceContext, desc)
, myDesc(std::move(desc))
{
    auto device = deviceContext->getDevice();

    char stringBuffer[128];

    myResources = std::make_shared<PipelineResourceView<Vk>>();
    myResources->sampler = createDefaultSampler(device);

    myCache = pipeline::loadPipelineCache(
        myDesc.cachePath,
        device,
        deviceContext->getPhysicalDeviceInfo().deviceProperties);
    
    static constexpr std::string_view pipelineCacheStr = "_PipelineCache";

    stbsp_sprintf(
        stringBuffer,
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

    myDescriptorPool = createDescriptorPool(device);

    deviceContext->addOwnedObject(
        0,
        VK_OBJECT_TYPE_DESCRIPTOR_POOL,
        reinterpret_cast<uint64_t>(myDescriptorPool),
        "Device_DescriptorPool");
}

template<>
Pipeline<Vk>::~Pipeline()
{
    auto [fileState, fileInfo] = pipeline::savePipelineCache(
        myDesc.cachePath,
        getDeviceContext()->getDevice(),
        getDeviceContext()->getPhysicalDeviceInfo().deviceProperties,
        myCache);

    for (const auto& pipelineIt : myPipelineMap)
        vkDestroyPipeline(getDeviceContext()->getDevice(), pipelineIt.second, nullptr);
    
    vkDestroyPipelineCache(getDeviceContext()->getDevice(), myCache, nullptr);
    vkDestroySampler(getDeviceContext()->getDevice(), myResources->sampler, nullptr);

    myResources.reset();
	myLayout.reset();
	myDescriptorSets.reset();

    if (myDescriptorPool)
        getDeviceContext()->addTimelineCallback(
            [device = getDeviceContext()->getDevice(), pool = myDescriptorPool](uint64_t){
                vkDestroyDescriptorPool(device, pool, nullptr);
        });
}
