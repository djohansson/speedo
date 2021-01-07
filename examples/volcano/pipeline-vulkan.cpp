#include "pipeline.h"
#include "vk-utils.h"

#include <stb_sprintf.h>

#include <cereal/archives/binary.hpp>

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
uint64_t PipelineContext<Vk>::internalCalculateHashKey(PipelineLayoutHandle<Vk> layoutHandle) const
{
    ZoneScopedN("Pipeline::internalCalculateHashKey");

    constexpr XXH64_hash_t seed = 42;
    auto result = XXH3_64bits_reset_withSeed(myXXHState.get(), seed);
    assert(result != XXH_ERROR);

    result = XXH3_64bits_update(myXXHState.get(), &layoutHandle, sizeof(layoutHandle));
    assert(result != XXH_ERROR);

    // todo: hash more state...

    return XXH3_64bits_digest(myXXHState.get());
}

template <>
PipelineHandle<Vk> PipelineContext<Vk>::internalCreateGraphicsPipeline(uint64_t hashKey)
{
    myShaderStages.clear();
    for (const auto& shader : myCurrentLayout->getShaders())
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

    myRenderPassAndFramebuffer = myResources->renderTarget->renderPassAndFramebuffer();
    mySubpass = myResources->renderTarget->getSubpass().value_or(0);

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
    pipelineInfo.layout = getCurrentLayout();
    pipelineInfo.renderPass = std::get<0>(myRenderPassAndFramebuffer);
    pipelineInfo.subpass = mySubpass;
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
PipelineContext<Vk>::PipelineMapConstIterator PipelineContext<Vk>::internalUpdateMap()
{
    auto hashKey = internalCalculateHashKey(getCurrentLayout());
    auto insertResult = myPipelineMap.insert(std::make_pair(hashKey, PipelineMapValueType{}));

    if (insertResult.second)
        insertResult.first->second.store(
            internalCreateGraphicsPipeline(hashKey),
            std::memory_order_relaxed);

    return insertResult.first;
}

template<>
const PipelineContext<Vk>::PipelineMapValueType& PipelineContext<Vk>::internalGetPipeline()
{
    auto& pipelineHandle = internalUpdateMap()->second;

    //internalUpdateMap()->second.wait(nullptr); todo: c++20
    while (!pipelineHandle.load(std::memory_order_relaxed));
    
    return pipelineHandle;
}

template<>
const DescriptorSetHandle<Vk>& PipelineContext<Vk>::internalGetDescriptorSet(uint8_t set) const
{
    const auto& setOptionalTuple = myDescriptorSets[set];
    assert(setOptionalTuple);
    const auto& setTuple = setOptionalTuple.value();

    return std::get<0>(setTuple)[std::get<1>(setTuple)];
}

template<>
uint64_t PipelineContext<Vk>::internalMakeDescriptorKey(const PipelineLayout<Vk>& layout, uint8_t set)
{
    static_assert(static_cast<uint32_t>(DescriptorSetCategory::Count) <= 4);
    assert(set < static_cast<uint32_t>(DescriptorSetCategory::Count));
    assert((reinterpret_cast<uint64_t>(&layout) & 0x3) == 0);
    
    return reinterpret_cast<uint64_t>(&layout) | (static_cast<uint64_t>(set) & 0x3);
}

template <>
void PipelineContext<Vk>::bind(CommandBufferHandle<Vk> cmd)
{
    vkCmdBindPipeline(cmd, myBindPoint, *this);
}

template <>
void PipelineContext<Vk>::bindDescriptorSet(
    CommandBufferHandle<Vk> cmd,
    uint8_t set,
    std::optional<uint32_t> bufferOffset) const
{
    vkCmdBindDescriptorSets(
        cmd,
        myBindPoint,
        getCurrentLayout(),
        set,
        1,
        &internalGetDescriptorSet(set),
        bufferOffset ? 1 : 0,
        bufferOffset ? &bufferOffset.value() : nullptr);
}

template<>
void PipelineContext<Vk>::setCurrentLayout(PipelineLayoutHandle<Vk> handle)
{
    myCurrentLayout = myLayouts.find(handle);
    assert(myCurrentLayout != myLayouts.cend());
}

// template <>
// void Pipeline<Vk>::copyDescriptorSet(uint8_t set, DescriptorSetArray<Vk>& dst) const
// {
//     if (const auto& bindings = myDescriptorData.at(internalMakeDescriptorKey(*myCurrentLayout, set)); !bindings.empty())//     {
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
void PipelineContext<Vk>::pushDescriptorSet(CommandBufferHandle<Vk> cmd, uint8_t set) const
{
    if (const auto& bindings = myDescriptorData.at(internalMakeDescriptorKey(*myCurrentLayout, set)); !bindings.empty())
    {
        std::vector<WriteDescriptorSet<Vk>> descriptorWrites;
        descriptorWrites.reserve(bindings.size());

        for (const auto& bindingPair : bindings)
        {
            auto bindingIndex = std::get<0>(bindingPair);
            const auto& binding = std::get<1>(bindingPair);
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
            VK_PIPELINE_BIND_POINT_GRAPHICS,
            getCurrentLayout(),
            set,
            static_cast<uint32_t>(descriptorWrites.size()),
            descriptorWrites.data());
    }
}

template <>
void PipelineContext<Vk>::writeDescriptorSet(uint8_t set) const
{
    if (const auto& bindings = myDescriptorData.at(internalMakeDescriptorKey(*myCurrentLayout, set)); !bindings.empty())
    {
        auto setHandle = internalGetDescriptorSet(set);

        std::vector<WriteDescriptorSet<Vk>> descriptorWrites;
        descriptorWrites.reserve(bindings.size());

        for (const auto& bindingPair : bindings)
        {
            auto bindingIndex = std::get<0>(bindingPair);
            const auto& binding = std::get<1>(bindingPair);
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

        vkUpdateDescriptorSets(
            getDeviceContext()->getDevice(),
            static_cast<uint32_t>(descriptorWrites.size()),
            descriptorWrites.data(),
            0,
            nullptr);
    }
}

template<>
PipelineLayoutHandle<Vk> PipelineContext<Vk>::emplaceLayout(PipelineLayout<Vk>&& layout)
{
    auto emplaceResult = myLayouts.emplace(std::move(layout));
    myCurrentLayout = emplaceResult.first;
    // temp!
    myDescriptorSets[0] = std::make_optional(std::make_tuple(DescriptorSetArray<Vk>(
        getDeviceContext(),
        myCurrentLayout->getDescriptorSetLayouts().at(0),
        DescriptorSetArrayCreateDesc<Vk>{
            {"DescriptorSetArray"},
            myDescriptorPool}), 0));
    myDescriptorSets[2] = std::make_optional(std::make_tuple(DescriptorSetArray<Vk>(
        getDeviceContext(),
        myCurrentLayout->getDescriptorSetLayouts().at(2),
        DescriptorSetArrayCreateDesc<Vk>{
            {"DescriptorSetArray"},
            myDescriptorPool}), 0));
    // myDescriptorSets[3] = std::make_optional(std::make_tuple(DescriptorSetArray<Vk>(
    //     getDeviceContext(),
    //     myCurrentLayout->getDescriptorSetLayouts().at(3),
    //     DescriptorSetArrayCreateDesc<Vk>{
    //         {"DescriptorSetArray"},
    //         myDescriptorPool}), 0));
    //
    return *myCurrentLayout;
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

    myResources = std::make_shared<PipelineResourceView<Vk>>();
    myResources->sampler = createDefaultSampler(device);

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
    vkDestroySampler(getDeviceContext()->getDevice(), myResources->sampler, nullptr);

    myResources.reset();
	// myLayout.reset();
	myDescriptorSets = {};

    if (myDescriptorPool)
        getDeviceContext()->addTimelineCallback(
            [device = getDeviceContext()->getDevice(), pool = myDescriptorPool](uint64_t){
                vkDestroyDescriptorPool(device, pool, nullptr);
        });
}
