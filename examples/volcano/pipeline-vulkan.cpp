#include "pipeline.h"
#include "vk-utils.h"

#include <cereal/archives/binary.hpp>

#include <stb_sprintf.h>

#include <xxhash.h>

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
		cereal::BinaryInputArchive bin(stream);
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

	return createPipelineCache(device, cacheData);
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
			
			cereal::BinaryOutputArchive bin(stream);
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

static PFN_vkCmdPushDescriptorSetKHR vkCmdPushDescriptorSetKHR = {};

}

template <>
PipelineLayout<Vk>& PipelineLayout<Vk>::operator=(PipelineLayout<Vk>&& other)
{
    DeviceResource<Vk>::operator=(std::move(other));
    myShaders = std::move(other.myShaders);
    myDescriptorSetLayouts = std::move(other.myDescriptorSetLayouts);
    myLayout = std::exchange(other.myLayout, {});
    return *this;
}

template <>
PipelineLayout<Vk>::PipelineLayout(PipelineLayout<Vk>&& other)
: DeviceResource<Vk>(std::move(other))
, myShaders(std::move(other.myShaders))
, myDescriptorSetLayouts(std::move(other.myDescriptorSetLayouts))
, myLayout(std::exchange(other.myLayout, {}))
{
}

template <>
PipelineLayout<Vk>::PipelineLayout(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    std::vector<ShaderModule<Vk>>&& shaderModules,
    DescriptorSetLayoutMap<Vk>&& descriptorSetLayouts,
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
    DescriptorSetLayoutMap<Vk>&& descriptorSetLayouts)
: PipelineLayout(
    deviceContext,
    std::move(shaderModules),
    std::move(descriptorSetLayouts),
    [&descriptorSetLayouts, device = deviceContext->getDevice()]
    {
        // todo: clean up this mess
        auto handles = descriptorset::getDescriptorSetLayoutHandles<Vk>(descriptorSetLayouts);
        auto pushConstantRanges = descriptorset::getPushConstantRanges<Vk>(descriptorSetLayouts);

        return createPipelineLayout(
            device,
            handles.data(),
            handles.size(),
            pushConstantRanges.data(),
            pushConstantRanges.size());
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
        DescriptorSetLayoutMap<Vk> map;
        for (auto layout : slangModule->layouts)
            map.emplace(
                std::make_pair(
                    layout.first,
                    DescriptorSetLayout<Vk>(
                        deviceContext,
                        std::move(layout.second))));
        return map;
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
uint64_t PipelineContext<Vk>::internalCalculateHashKey() const
{
    ZoneScopedN("Pipeline::internalCalculateHashKey");

    thread_local std::unique_ptr<XXH3_state_t, XXH_errorcode(*)(XXH3_state_t*)> threadXXHState =
    {
        XXH3_createState(),
        XXH3_freeState
    };

    auto result = XXH3_64bits_reset(threadXXHState.get());
    assert(result != XXH_ERROR);

    result = XXH3_64bits_update(threadXXHState.get(), &myBindPoint, sizeof(myBindPoint));
    assert(result != XXH_ERROR);

    auto layoutHandle = static_cast<PipelineLayoutHandle<Vk>>(*getLayout());
    result = XXH3_64bits_update(threadXXHState.get(), &layoutHandle, sizeof(layoutHandle));
    assert(result != XXH_ERROR);

    // todo: hash more state...

    // todo: rendertargets need to use hash key derived from its state and not its handles/pointers, since they are recreated often
    // auto [renderPassHandle, frameBufferHandle] =
    //     static_cast<RenderTarget<Vk>::ValueType>(*getRenderTarget());
    // result = XXH3_64bits_update(threadXXHState.get(), &renderPassHandle, sizeof(renderPassHandle));
    // result = XXH3_64bits_update(threadXXHState.get(), &frameBufferHandle, sizeof(frameBufferHandle));
    // assert(result != XXH_ERROR);

    return XXH3_64bits_digest(threadXXHState.get());
}

template <>
void PipelineContext<Vk>::internalResetSharedState()
{
    myShaderStages.clear();

    if (getLayout())
    {
        for (const auto& shader : getLayout()->getShaders())
            myShaderStages.emplace_back(
                PipelineShaderStageCreateInfo<Vk>{
                    VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                    nullptr,
                    0,
                    shader.getEntryPoint().second,
                    shader,
                    shader.getEntryPoint().first.c_str(),
                    nullptr});

        for (const auto& [set, layout] : getLayout()->getDescriptorSetLayouts())
            if (layout.getDesc().flags ^ VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR)
                myDescriptorSets.emplace(
                    std::make_pair(
                        set,
                        DescriptorSetArray<Vk>(
                            getDeviceContext(),
                            layout,
                            DescriptorSetArrayCreateDesc<Vk>{
                                {"DescriptorSetArray"},
                                myDescriptorPool})));
    }
}

template <>
void PipelineContext<Vk>::internalResetGraphicsInputState()
{
    myGraphicsState.vertexInput = {
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        nullptr,
        0,
        0,
        nullptr,
        0,
        nullptr};

    myGraphicsState.inputAssembly = {
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        nullptr,
        0,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        VK_FALSE};
}

template <>
void PipelineContext<Vk>::internalResetGraphicsRasterizationState()
{
    myGraphicsState.viewports.clear();
    myGraphicsState.viewports.emplace_back(Viewport<Vk>{0.0f, 0.0f, 0, 0, 0.0f, 1.0f});

    myGraphicsState.scissorRects.clear();
    myGraphicsState.scissorRects.emplace_back(Rect2D<Vk>{{0, 0}, {0, 0}});

    myGraphicsState.viewport = {
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        nullptr,
        0,
        static_cast<uint32_t>(myGraphicsState.viewports.size()),
        myGraphicsState.viewports.data(),
        static_cast<uint32_t>(myGraphicsState.scissorRects.size()),
        myGraphicsState.scissorRects.data()};

    myGraphicsState.rasterization = {
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

    myGraphicsState.multisample = {
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        nullptr,
        0,
        VK_SAMPLE_COUNT_1_BIT,
        VK_FALSE,
        1.0f,
        nullptr,
        VK_FALSE,
        VK_FALSE};
}

template <>
void PipelineContext<Vk>::internalResetGraphicsOutputState()
{
    myGraphicsState.depthStencil = {
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

    myGraphicsState.colorBlendAttachments.clear();
    myGraphicsState.colorBlendAttachments.emplace_back(
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
    
    myGraphicsState.colorBlend = {
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        nullptr,
        0,
        VK_FALSE,
        VK_LOGIC_OP_COPY,
        static_cast<uint32_t>(myGraphicsState.colorBlendAttachments.size()),
        myGraphicsState.colorBlendAttachments.data(),
        {0.0f, 0.0f, 0.0f, 0.0f}};
}
    
template <>
void PipelineContext<Vk>::internalResetGraphicsDynamicState()
{
    myGraphicsState.dynamicStateDescs.clear();
    myGraphicsState.dynamicStateDescs.emplace_back(VK_DYNAMIC_STATE_VIEWPORT);
    myGraphicsState.dynamicStateDescs.emplace_back(VK_DYNAMIC_STATE_SCISSOR);
    
    myGraphicsState.dynamicState = {
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        nullptr,
        0,
        static_cast<uint32_t>(myGraphicsState.dynamicStateDescs.size()),
        myGraphicsState.dynamicStateDescs.data()};
}

template <>
PipelineHandle<Vk> PipelineContext<Vk>::internalCreateGraphicsPipeline(uint64_t hashKey)
{
    VkGraphicsPipelineCreateInfo pipelineInfo = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pipelineInfo.stageCount = static_cast<uint32_t>(myShaderStages.size());
    pipelineInfo.pStages = myShaderStages.data();
    pipelineInfo.pVertexInputState = &myGraphicsState.vertexInput;
    pipelineInfo.pInputAssemblyState = &myGraphicsState.inputAssembly;
    pipelineInfo.pViewportState = &myGraphicsState.viewport;
    pipelineInfo.pRasterizationState = &myGraphicsState.rasterization;
    pipelineInfo.pMultisampleState = &myGraphicsState.multisample;
    pipelineInfo.pDepthStencilState = &myGraphicsState.depthStencil;
    pipelineInfo.pColorBlendState = &myGraphicsState.colorBlend;
    pipelineInfo.pDynamicState = &myGraphicsState.dynamicState;
    pipelineInfo.layout = *getLayout();
    pipelineInfo.renderPass = *getRenderTarget();
    pipelineInfo.subpass = getRenderTarget()->getSubpass().value_or(0);
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

template<>
PipelineHandle<Vk> PipelineContext<Vk>::internalGetPipeline()
{
    auto hashKey = internalCalculateHashKey();
    
    auto insertResult = myPipelineMap.insert({hashKey, nullptr});

    if (insertResult.second)
        insertResult.first->second.store(
            internalCreateGraphicsPipeline(hashKey),
            std::memory_order_relaxed);

    auto& pipelineHandle = insertResult.first->second;

    while (!pipelineHandle.load(std::memory_order_relaxed)); //pipelineHandle.wait(nullptr); todo: c++20
    
    return pipelineHandle;
}

template<>
DescriptorSetHandle<Vk> PipelineContext<Vk>::internalGetDescriptorSet(uint32_t set) const
{
    // todo:
    return myDescriptorSets.at(set)[0];
}

template <>
void PipelineContext<Vk>::bind(CommandBufferHandle<Vk> cmd)
{
    vkCmdBindPipeline(cmd, myBindPoint, internalGetPipeline());
}

template <>
void PipelineContext<Vk>::bindDescriptorSet(
    CommandBufferHandle<Vk> cmd,
    uint32_t set,
    std::optional<uint32_t> bufferOffset) const
{
    auto descriptorSetHandle = internalGetDescriptorSet(set);
    vkCmdBindDescriptorSets(
        cmd,
        myBindPoint,
        *getLayout(),
        set,
        1,
        &descriptorSetHandle,
        bufferOffset ? 1 : 0,
        bufferOffset ? &bufferOffset.value() : nullptr);
}

template<>
void PipelineContext<Vk>::setModel(const std::shared_ptr<Model<Vk>>& model)
{
    myGraphicsState.vertexInput.vertexBindingDescriptionCount = static_cast<uint32_t>(model->getBindings().size());
    myGraphicsState.vertexInput.pVertexBindingDescriptions = model->getBindings().data();
    myGraphicsState.vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(model->getDesc().attributes.size());
    myGraphicsState.vertexInput.pVertexAttributeDescriptions = model->getDesc().attributes.data();

    myGraphicsState.resources.model = model;
}

template<>
void PipelineContext<Vk>::setLayout(const std::shared_ptr<PipelineLayout<Vk>>& layout)
{
    myLayout = layout;

    internalResetSharedState();
}

template<>
void PipelineContext<Vk>::setRenderTarget(const std::shared_ptr<RenderTarget<Vk>>& renderTarget)
{
    auto extent = renderTarget ? renderTarget->getRenderTargetDesc().extent : Extent2d<Vk>{ 0, 0 };
    
    myGraphicsState.viewports[0].width = static_cast<float>(extent.width);
    myGraphicsState.viewports[0].height = static_cast<float>(extent.height);
    myGraphicsState.scissorRects[0].offset = {0, 0};
    myGraphicsState.scissorRects[0].extent = {extent.width, extent.height};

    myRenderTarget = renderTarget;
}

// template <>
// void Pipeline<Vk>::copyDescriptorSet(uint32_t set, DescriptorSetArray<Vk>& dst) const
// {
//     if (const auto& bindings = myDescriptorMap.at(getLayout()->getDescriptorSetLayouts().at(set).getKey()); !bindings.empty())//     {
//         std::vector<CopyDescriptorSet<Vk>> descriptorCopies;
//         descriptorCopies.reserve(bindings.size());

//         for (const auto& binding : bindings)
//         {
//             auto srcSetHandle = myDescriptorSets[set];
//             auto dstSetHandle = dst[set];
//             auto bindingIt = static_cast<uint32_t>(&binding - &bindings[0]);
//             const auto& variantVector = std::get<1>(binding);

//             descriptorCopies.emplace_back(std::visit(pipeline::overloaded{
//                 [srcSetHandle, dstSetHandle, bindingIt](
//                     const std::vector<DescriptorBufferInfo<Vk>>& bufferInfos){
//                         return CopyDescriptorSet<Vk>{
//                             VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET,
//                             nullptr,
//                             srcSetHandle,
//                             bindingIt,
//                             0,
//                             dstSetHandle,
//                             bindingIt,
//                             0,
//                             static_cast<uint32_t>(bufferInfos.size())};
//                 },
//                 [srcSetHandle, dstSetHandle, bindingIt](
//                     const std::vector<DescriptorImageInfo<Vk>>& imageInfos){
//                         return CopyDescriptorSet<Vk>{
//                             VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET,
//                             nullptr,
//                             srcSetHandle,
//                             bindingIt,
//                             0,
//                             dstSetHandle,
//                             bindingIt,
//                             0,
//                             static_cast<uint32_t>(imageInfos.size())};
//                 },
//                 [srcSetHandle, dstSetHandle, bindingIt](
//                     const std::vector<BufferViewHandle<Vk>>& bufferViews){
//                         return CopyDescriptorSet<Vk>{
//                             VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET,
//                             nullptr,
//                             srcSetHandle,
//                             bindingIt,
//                             0,
//                             dstSetHandle,
//                             bindingIt,
//                             0,
//                             static_cast<uint32_t>(bufferViews.size())};
//                 },
//             }, variantVector));
//         }

//         vkUpdateDescriptorSets(
//             getDeviceContext()->getDevice(),
//             0,
//             nullptr,
//             static_cast<uint32_t>(descriptorCopies.size()),
//             descriptorCopies.data());
//     }
// }

template <>
void PipelineContext<Vk>::pushDescriptorSet(CommandBufferHandle<Vk> cmd, uint32_t set) const
{
    if (const auto& bindings = myDescriptorMap.at(getLayout()->getDescriptorSetLayouts().at(set).getKey()); !bindings.empty())
    {
        std::vector<WriteDescriptorSet<Vk>> descriptorWrites;
        descriptorWrites.reserve(bindings.size());

        for (const auto& bindingPair : bindings)
        {
            auto bindingIndex = bindingPair.first;
            const auto& binding = bindingPair.second;
            auto bindingType = std::get<0>(binding);
            const auto& variantVector = std::get<1>(binding);

            descriptorWrites.emplace_back(std::visit(pipeline::overloaded{
                [bindingType, bindingIndex](
                const std::vector<DescriptorBufferInfo<Vk>>& bufferInfos){
                    return WriteDescriptorSet<Vk>{
                        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                        nullptr,
                        0,
                        bindingIndex,
                        0,
                        static_cast<uint32_t>(bufferInfos.size()),
                        bindingType,
                        nullptr,
                        bufferInfos.data(),
                        nullptr};
                },
                [bindingType, bindingIndex](
                const std::vector<DescriptorImageInfo<Vk>>& imageInfos){
                    return WriteDescriptorSet<Vk>{
                        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                        nullptr,
                        0,
                        bindingIndex,
                        0,
                        static_cast<uint32_t>(imageInfos.size()),
                        bindingType,
                        imageInfos.data(),
                        nullptr,
                        nullptr};
                },
                [bindingType, bindingIndex](
                const std::vector<BufferViewHandle<Vk>>& bufferViews){
                    return WriteDescriptorSet<Vk>{
                        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                        nullptr,
                        0,
                        bindingIndex,
                        0,
                        static_cast<uint32_t>(bufferViews.size()),
                        bindingType,
                        nullptr,
                        nullptr,
                        bufferViews.data()};
                },
                [bindingType, bindingIndex](
                const std::vector<InlineUniformBlock<Vk>>& parameterBlocks){
                    assert(parameterBlocks.size() == 1);
                    return WriteDescriptorSet<Vk>{
                        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                        parameterBlocks.data(),
                        0,
                        bindingIndex,
                        0,
                        parameterBlocks[0].dataSize,
                        bindingType,
                        nullptr,
                        nullptr,
                        nullptr};
                },
            }, variantVector));
        }

        if (!pipeline::vkCmdPushDescriptorSetKHR)
            pipeline::vkCmdPushDescriptorSetKHR = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(
                vkGetDeviceProcAddr(
                    getDeviceContext()->getDevice(),
                    "vkCmdPushDescriptorSetKHR"));

        pipeline::vkCmdPushDescriptorSetKHR(
            cmd,
            myBindPoint,
            *getLayout(),
            set,
            static_cast<uint32_t>(descriptorWrites.size()),
            descriptorWrites.data());
    }
}

template <>
void PipelineContext<Vk>::writeDescriptorSet(uint32_t set) const
{
    if (const auto& bindings = myDescriptorMap.at(getLayout()->getDescriptorSetLayouts().at(set).getKey()); !bindings.empty())
    {
        auto setHandle = internalGetDescriptorSet(set);

        std::vector<WriteDescriptorSet<Vk>> descriptorWrites;
        descriptorWrites.reserve(bindings.size());

        for (const auto& bindingPair : bindings)
        {
            auto bindingIndex = bindingPair.first;
            const auto& binding = bindingPair.second;
            auto bindingType = std::get<0>(binding);
            const auto& variantVector = std::get<1>(binding);

            descriptorWrites.emplace_back(std::visit(pipeline::overloaded{
                [setHandle, bindingType, bindingIndex](
                    const std::vector<DescriptorBufferInfo<Vk>>& bufferInfos){
                        return WriteDescriptorSet<Vk>{
                            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                            nullptr,
                            setHandle,
                            bindingIndex,
                            0,
                            static_cast<uint32_t>(bufferInfos.size()),
                            bindingType,
                            nullptr,
                            bufferInfos.data(),
                            nullptr};
                },
                [setHandle, bindingType, bindingIndex](
                    const std::vector<DescriptorImageInfo<Vk>>& imageInfos){
                        return WriteDescriptorSet<Vk>{
                            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                            nullptr,
                            setHandle,
                            bindingIndex,
                            0,
                            static_cast<uint32_t>(imageInfos.size()),
                            bindingType,
                            imageInfos.data(),
                            nullptr,
                            nullptr};
                },
                [setHandle, bindingType, bindingIndex](
                    const std::vector<BufferViewHandle<Vk>>& bufferViews){
                        return WriteDescriptorSet<Vk>{
                            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                            nullptr,
                            setHandle,
                            bindingIndex,
                            0,
                            static_cast<uint32_t>(bufferViews.size()),
                            bindingType,
                            nullptr,
                            nullptr,
                            bufferViews.data()};
                },
                [setHandle, bindingType, bindingIndex](
                    const std::vector<InlineUniformBlock<Vk>>& parameterBlocks){
                        assert(parameterBlocks.size() == 1);
                        return WriteDescriptorSet<Vk>{
                            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                            parameterBlocks.data(),
                            setHandle,
                            bindingIndex,
                            0,
                            parameterBlocks[0].dataSize,
                            bindingType,
                            nullptr,
                            nullptr,
                            nullptr};
                },
            }, variantVector));
        }

        // todo: increment active set to avoid writing to in-flight set.

        vkUpdateDescriptorSets(
            getDeviceContext()->getDevice(),
            static_cast<uint32_t>(descriptorWrites.size()),
            descriptorWrites.data(),
            0,
            nullptr);
    }
}

template<>
PipelineContext<Vk>::PipelineContext(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    AutoSaveJSONFileObject<PipelineConfiguration<Vk>>&& config)
: DeviceResource(deviceContext, config)
, myConfig(std::move(config))
{
    auto device = deviceContext->getDevice();

    char stringBuffer[128];

    myCache = pipeline::loadPipelineCache(
        myConfig.cachePath,
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

    // temp
    myGraphicsState.resources.sampler = createDefaultSampler(device);
    //

    internalResetSharedState();
    internalResetGraphicsInputState();
    internalResetGraphicsRasterizationState();
    internalResetGraphicsOutputState();
    internalResetGraphicsDynamicState();
}

template<>
PipelineContext<Vk>::~PipelineContext()
{
    auto [fileState, fileInfo] = pipeline::savePipelineCache(
        myConfig.cachePath,
        getDeviceContext()->getDevice(),
        getDeviceContext()->getPhysicalDeviceInfo().deviceProperties,
        myCache);

    for (const auto& pipelineIt : myPipelineMap)
        vkDestroyPipeline(getDeviceContext()->getDevice(), pipelineIt.second, nullptr);
    
    vkDestroyPipelineCache(getDeviceContext()->getDevice(), myCache, nullptr);
    vkDestroySampler(getDeviceContext()->getDevice(), myGraphicsState.resources.sampler, nullptr);

    myDescriptorSets.clear();
    
    myGraphicsState.resources = {};

    if (myDescriptorPool)
        getDeviceContext()->addTimelineCallback(
            [device = getDeviceContext()->getDevice(), pool = myDescriptorPool](uint64_t){
                vkDestroyDescriptorPool(device, pool, nullptr);
        });
}
