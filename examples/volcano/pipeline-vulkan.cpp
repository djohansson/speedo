#include "pipeline.h"
#include "vk-utils.h"
#include "volcano.h"

#include <cereal/archives/binary.hpp>

#include <stb_sprintf.h>

#pragma pack(push, 1)
template <>
struct PipelineCacheHeader<Vk>
{
	uint32_t headerLength = 0ul;
	uint32_t cacheHeaderVersion = 0ul;
	uint32_t vendorID = 0ul;
	uint32_t deviceID = 0ul;
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

static PFN_vkCmdPushDescriptorSetWithTemplateKHR vkCmdPushDescriptorSetWithTemplateKHR = {};

}

template <>
PipelineLayout<Vk>& PipelineLayout<Vk>::operator=(PipelineLayout<Vk>&& other) noexcept
{
    DeviceObject::operator=(std::forward<PipelineLayout<Vk>>(other));
    myShaderModules = std::exchange(other.myShaderModules, {});
    myDescriptorSetLayouts = std::exchange(other.myDescriptorSetLayouts, {});
    myLayout = std::exchange(other.myLayout, {});
    return *this;
}

template <>
PipelineLayout<Vk>::PipelineLayout(PipelineLayout<Vk>&& other) noexcept
: DeviceObject(std::forward<PipelineLayout<Vk>>(other))
, myShaderModules(std::exchange(other.myShaderModules, {}))
, myDescriptorSetLayouts(std::exchange(other.myDescriptorSetLayouts, {}))
, myLayout(std::exchange(other.myLayout, {}))
{
}

template <>
PipelineLayout<Vk>::PipelineLayout(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    std::vector<ShaderModule<Vk>>&& shaderModules,
    DescriptorSetLayoutFlatMap<Vk>&& descriptorSetLayouts,
    PipelineLayoutHandle<Vk>&& layout)
: DeviceObject(
    deviceContext,
    {"_PipelineLayout"},
    1,
    VK_OBJECT_TYPE_PIPELINE_LAYOUT,
    reinterpret_cast<uint64_t*>(&layout))
, myShaderModules(std::exchange(shaderModules, {}))
, myDescriptorSetLayouts(std::exchange(descriptorSetLayouts, {}))
, myLayout(std::forward<PipelineLayoutHandle<Vk>>(layout))
{
}

template <>
PipelineLayout<Vk>::PipelineLayout(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    std::vector<ShaderModule<Vk>>&& shaderModules,
    DescriptorSetLayoutFlatMap<Vk>&& descriptorSetLayouts)
: PipelineLayout(
    deviceContext,
    std::forward<std::vector<ShaderModule<Vk>>>(shaderModules),
    std::forward<DescriptorSetLayoutFlatMap<Vk>>(descriptorSetLayouts),
    [&descriptorSetLayouts, device = deviceContext->getDevice()]
    {
        // todo: rewrite flatmap so that keys and vals are stored as separate arrays so that we dont have to make this conversion
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
    const ShaderSet<Vk>& shaderSet)
: PipelineLayout(
    deviceContext,
    [&shaderSet, &deviceContext]
    {
        std::vector<ShaderModule<Vk>> shaderModules;
        shaderModules.reserve(shaderSet.shaders.size());
        for (const auto& shader : shaderSet.shaders)
            shaderModules.emplace_back(ShaderModule<Vk>(deviceContext, shader));
        return shaderModules;
    }(),
    [&shaderSet, &deviceContext]
    {
        DescriptorSetLayoutFlatMap<Vk> map;
        for (auto [set, layout] : shaderSet.layouts)
            map.emplace(
                map.end(),
                std::make_pair(
                    set,
                    DescriptorSetLayout<Vk>(
                        deviceContext,
                        std::move(layout))));
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

template <>
void PipelineLayout<Vk>::swap(PipelineLayout& rhs) noexcept
{
    DeviceObject::swap(rhs);
    std::swap(myShaderModules, rhs.myShaderModules);
    std::swap(myDescriptorSetLayouts, rhs.myDescriptorSetLayouts);
    std::swap(myLayout, rhs.myLayout);
}

template<>
uint64_t PipelineContext<Vk>::internalCalculateHashKey() const
{
    ZoneScopedN("PipelineContext::internalCalculateHashKey");

    thread_local std::unique_ptr<XXH3_state_t, XXH_errorcode(*)(XXH3_state_t*)> threadXXHState =
    {
        XXH3_createState(),
        XXH3_freeState
    };

    auto result = XXH3_64bits_reset(threadXXHState.get());
    assert(result != XXH_ERROR);

    result = XXH3_64bits_update(threadXXHState.get(), &myBindPoint, sizeof(myBindPoint));
    assert(result != XXH_ERROR);

    auto layoutHandle = static_cast<PipelineLayoutHandle<Vk>>(getLayout());
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
void PipelineContext<Vk>::internalPrepareDescriptorSets()
{
    const auto& layout = getLayout();

    for (const auto& [set, setLayout] : layout.getDescriptorSetLayouts())
    {
        auto insertResultPair = myDescriptorMap.emplace(
            static_cast<DescriptorSetLayoutHandle<Vk>>(setLayout),
            std::make_tuple(
                UpgradableSharedMutex<>{},
                DescriptorSetStatus::Ready,
                BindingsMap<Vk>{},
                BindingsData<Vk>{},
                DescriptorUpdateTemplate<Vk>{
                    getDeviceContext(),
                    DescriptorUpdateTemplateCreateDesc<Vk>
                    {
                        setLayout.getDesc().flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR ?
                            VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR :
                            VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET,
                        static_cast<VkDescriptorSetLayout>(setLayout),
                        myBindPoint,
                        static_cast<VkPipelineLayout>(layout),
                        set
                    }},
                setLayout.getDesc().flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR ?
                    std::nullopt :
                    std::make_optional(DescriptorSetArrayList<Vk>{})));

        auto& [insertIt, insertResult] = insertResultPair;
        auto& [bindingIndex, bindingTuple] = *insertIt;
        auto& [mutex, setState, bindingsMap, bindingsData, setTemplate, setOptionalArrayList] = bindingTuple;

        if (setOptionalArrayList)
        {
            auto& setArrayList = setOptionalArrayList.value();
            
            setArrayList.emplace_front(
                std::make_tuple(
                    DescriptorSetArray<Vk>(
                        getDeviceContext(),
                        setLayout,
                        DescriptorSetArrayCreateDesc<Vk>{myDescriptorPool}),
                    ~0,
                    0));
        }
    }
}

template <>
void PipelineContext<Vk>::internalResetGraphicsState()
{
    myGraphicsState.shaderStages.clear();
    myGraphicsState.shaderStageFlags = {};

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
void PipelineContext<Vk>::internalResetComputeState()
{
    //myComputeState....
}

template <>
void PipelineContext<Vk>::internalResetState()
{
    internalResetGraphicsState();
    internalResetComputeState();
}

template <>
PipelineHandle<Vk> PipelineContext<Vk>::internalCreateGraphicsPipeline(uint64_t hashKey)
{
    ZoneScopedN("PipelineContext::internalCreateGraphicsPipeline");

    const auto& layout = getLayout();
    auto& renderTarget = getRenderTarget();

    VkGraphicsPipelineCreateInfo pipelineInfo = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pipelineInfo.stageCount = static_cast<uint32_t>(myGraphicsState.shaderStages.size());
    pipelineInfo.pStages = myGraphicsState.shaderStages.data();
    pipelineInfo.pVertexInputState = &myGraphicsState.vertexInput;
    pipelineInfo.pInputAssemblyState = &myGraphicsState.inputAssembly;
    pipelineInfo.pViewportState = &myGraphicsState.viewport;
    pipelineInfo.pRasterizationState = &myGraphicsState.rasterization;
    pipelineInfo.pMultisampleState = &myGraphicsState.multisample;
    pipelineInfo.pDepthStencilState = &myGraphicsState.depthStencil;
    pipelineInfo.pColorBlendState = &myGraphicsState.colorBlend;
    pipelineInfo.pDynamicState = &myGraphicsState.dynamicState;
    pipelineInfo.layout = layout;
    pipelineInfo.renderPass = renderTarget;
    pipelineInfo.subpass = renderTarget.getSubpass().value_or(0);
    pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
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

    getDeviceContext()->addOwnedObjectHandle(
        getUid(),
        VK_OBJECT_TYPE_PIPELINE,
        reinterpret_cast<uint64_t>(pipelineHandle),
        stringBuffer);

    return pipelineHandle;
}

template<>
PipelineHandle<Vk> PipelineContext<Vk>::internalGetPipeline()
{
    ZoneScopedN("PipelineContext::internalGetPipeline");

    auto [keyValIt, insertResult] = myPipelineMap.insert({internalCalculateHashKey(), nullptr});
    auto& [key, pipelineHandleAtomic] = *keyValIt;
    
    if (insertResult)
    {
        ZoneScopedN("PipelineContext::internalGetPipeline::store");

        pipelineHandleAtomic.store(
            internalCreateGraphicsPipeline(key),
            std::memory_order_relaxed);
        pipelineHandleAtomic.notify_all();
    }
    else
    {
        ZoneScopedN("PipelineContext::internalGetPipeline::wait");

        pipelineHandleAtomic.wait(nullptr);
    }
    
    return pipelineHandleAtomic;
}

template <>
void PipelineContext<Vk>::bindPipeline(
    CommandBufferHandle<Vk> cmd,
    PipelineBindPoint<Vk> bindPoint,
    PipelineHandle<Vk> handle) const
{
    ZoneScopedN("PipelineContext::bindPipeline");

    vkCmdBindPipeline(cmd, bindPoint, handle);
}

template <>
void PipelineContext<Vk>::bindPipelineAuto(CommandBufferHandle<Vk> cmd)
{
    bindPipeline(cmd, myBindPoint, internalGetPipeline());
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
void PipelineContext<Vk>::setLayout(const std::shared_ptr<PipelineLayout<Vk>>& layout, PipelineBindPoint<Vk> bindPoint)
{
    assert(layout);

    myLayout = layout;
    myBindPoint = bindPoint;

    const auto& shaderModules = myLayout->getShaderModules();

    assert(!shaderModules.empty());

    switch (myBindPoint)
    {
    case VK_PIPELINE_BIND_POINT_GRAPHICS:
        {
            myGraphicsState.shaderStageFlags = {};
            myGraphicsState.shaderStages.reserve(shaderModules.size());

            for (const auto& shader : shaderModules)
            {
                auto& [entryPointName, shaderStage] = shader.getEntryPoint();

                if (shaderStage & VK_SHADER_STAGE_ALL_GRAPHICS)
                {
                    myGraphicsState.shaderStages.emplace_back(
                        PipelineShaderStageCreateInfo<Vk>{
                            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                            nullptr,
                            0,
                            shaderStage,
                            shader,
                            entryPointName.c_str(),
                            nullptr});

                    myGraphicsState.shaderStageFlags |= shaderStage;
                }
            }
        }
        break;
    case VK_PIPELINE_BIND_POINT_COMPUTE:
        break;
    default:
        assertf(false, "Not implemented");
        break;
    };

    internalPrepareDescriptorSets();
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

template <>
void PipelineContext<Vk>::internalUpdateDescriptorSet(
    const DescriptorSetLayout<Vk>& setLayout,
    const BindingsData<Vk>& bindingsData,
    const DescriptorUpdateTemplate<Vk>& setTemplate,
    DescriptorSetArrayList<Vk>& setArrayList)
{
    ZoneScopedN("PipelineContext::internalUpdateDescriptorSet");

    bool setArrayListIsEmpty = setArrayList.empty();
    bool frontArrayIsFull = setArrayListIsEmpty ?
        false :
        std::get<1>(setArrayList.front()) == (std::get<0>(setArrayList.front()).capacity() - 1);

    if (setArrayListIsEmpty || frontArrayIsFull)
    {
        setArrayList.emplace_front(
            std::make_tuple(
                DescriptorSetArray<Vk>(
                    getDeviceContext(),
                    setLayout,
                    DescriptorSetArrayCreateDesc<Vk>{myDescriptorPool}),
                ~0,
                0));
    }

    auto& [setArray, setIndex, setRefCount] = setArrayList.front();
    auto setHandle = setArray[++setIndex];

    {
        ZoneScopedN("PipelineContext::internalUpdateDescriptorSet::vkUpdateDescriptorSetWithTemplate");

        vkUpdateDescriptorSetWithTemplate(
            getDeviceContext()->getDevice(),
            setHandle,
            setTemplate,
            bindingsData.data());
    }

    // clean up
    if (auto setArrayIt = setArrayList.begin(); setArrayIt != setArrayList.end())
    {
        setArrayIt++;
        while (setArrayIt != setArrayList.end())
        {
            auto& [setArray, setIndex, setRefCount] = *setArrayIt;
            if (!setRefCount)
                setArrayIt = setArrayList.erase(setArrayIt);
            else
                setArrayIt++;
        }
    }
}

template <>
void PipelineContext<Vk>::internalPushDescriptorSet(
    CommandBufferHandle<Vk> cmd,
    const BindingsData<Vk>& bindingsData,
    const DescriptorUpdateTemplate<Vk>& setTemplate) const
{
    ZoneScopedN("PipelineContext::internalPushDescriptorSet");

    {
        ZoneScopedN("PipelineContext::internalPushDescriptorSet::vkCmdPushDescriptorSetWithTemplateKHR");

        if (!pipeline::vkCmdPushDescriptorSetWithTemplateKHR)
            pipeline::vkCmdPushDescriptorSetWithTemplateKHR = reinterpret_cast<PFN_vkCmdPushDescriptorSetWithTemplateKHR>(
                vkGetDeviceProcAddr(
                    getDeviceContext()->getDevice(),
                    "vkCmdPushDescriptorSetWithTemplateKHR"));

        pipeline::vkCmdPushDescriptorSetWithTemplateKHR(
            cmd,
            setTemplate,
            getLayout(),
            setTemplate.getDesc().set,
            bindingsData.data());
    }
}

template <>
void PipelineContext<Vk>::internalUpdateDescriptorSetTemplate(
    const BindingsMap<Vk>& bindingsMap,
    DescriptorUpdateTemplate<Vk>& setTemplate)
{
    ZoneScopedN("PipelineContext::internalUpdateDescriptorSetTemplate");

    std::vector<DescriptorUpdateTemplateEntry<Vk>> entries;
    entries.reserve(bindingsMap.size());

    for (const auto& [index, binding] : bindingsMap)
    {
        const auto& [offset, count, type, ranges] = binding;

        uint32_t rangeOffset = 0ul;

        for (const auto& [low, high] : ranges)
        {
            auto rangeCount = high - low;

            entries.emplace_back(DescriptorUpdateTemplateEntry<Vk>
            {
                index,
                low,
                rangeCount,
                type,
                (offset + rangeOffset) * sizeof(BindingVariant<Vk>),
                sizeof(BindingVariant<Vk>)
            });

            rangeOffset += rangeCount;
        }
    }
    
    setTemplate.setEntries(std::move(entries));
}

template <>
void PipelineContext<Vk>::bindDescriptorSet(
    CommandBufferHandle<Vk> cmd,
    DescriptorSetHandle<Vk> handle,
    PipelineBindPoint<Vk> bindPoint,
    PipelineLayoutHandle<Vk> layoutHandle,
    uint32_t set,
    std::optional<uint32_t> bufferOffset) const
{
    ZoneScopedN("PipelineContext::bindDescriptorSet");

    vkCmdBindDescriptorSets(
        cmd,
        bindPoint,
        layoutHandle,
        set,
        1,
        &handle,
        bufferOffset ? 1 : 0,
        bufferOffset ? &bufferOffset.value() : nullptr);
}

template <>
void PipelineContext<Vk>::bindDescriptorSetAuto(
    CommandBufferHandle<Vk> cmd,
    uint32_t set,
    std::optional<uint32_t> bufferOffset) 
{
    ZoneScopedN("PipelineContext::bindDescriptorSetAuto");

    const auto& layout = getLayout();
    const auto& setLayout = layout.getDescriptorSetLayout(set);
    auto& [mutex, setState, bindingsMap, bindingsData, setTemplate, setOptionalArrayList] = myDescriptorMap.at(setLayout);
    
    mutex.lock_upgrade();
    
    if (setState == DescriptorSetStatus::Dirty && setOptionalArrayList)
    {
        mutex.unlock_upgrade_and_lock();

        internalUpdateDescriptorSet(
            setLayout,
            bindingsData,
            setTemplate,
            setOptionalArrayList.value());

        setState = DescriptorSetStatus::Ready;

        mutex.unlock_and_lock_shared();
    }
    else [[likely]]
    {
        mutex.unlock_upgrade_and_lock_shared();
    }
    
    if (setOptionalArrayList)
    {
        auto& setArrayList = setOptionalArrayList.value();
        assert(!setArrayList.empty());
        auto& [setArray, setIndex, setRefCount] = setArrayList.front();
        auto handle = setArray[setIndex];

        bindDescriptorSet(
            cmd,
            handle,
            myBindPoint,
            static_cast<PipelineLayoutHandle<Vk>>(layout),
            set,
            bufferOffset);

        setRefCount++;

        getDeviceContext()->addTimelineCallback([refCountPtr = &setRefCount](uint64_t) {
            (*refCountPtr)--;
        });
    }
    else
    {
        internalPushDescriptorSet(cmd, bindingsData, setTemplate);
    }

    mutex.unlock_shared();
}

template<>
PipelineContext<Vk>::PipelineContext(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    PipelineConfiguration<Vk>&& defaultConfig)
: DeviceObject(deviceContext, {})
, myConfig(
    AutoSaveJSONFileObject<PipelineConfiguration<Vk>>(
        std::filesystem::path(volcano_getUserProfilePath()) / "pipeline.json",
        std::forward<PipelineConfiguration<Vk>>(defaultConfig)))
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

    deviceContext->addOwnedObjectHandle(
        getUid(),
        VK_OBJECT_TYPE_PIPELINE_CACHE,
        reinterpret_cast<uint64_t>(myCache),
        stringBuffer);

    myDescriptorPool = createDescriptorPool(device);

    deviceContext->addOwnedObjectHandle(
        getUid(),
        VK_OBJECT_TYPE_DESCRIPTOR_POOL,
        reinterpret_cast<uint64_t>(myDescriptorPool),
        "Device_DescriptorPool");

    // todo: refactor, since this will be called to excessivly
    internalResetState();
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

    myGraphicsState.resources = {};
    myDescriptorMap.clear();

    if (myDescriptorPool)
        getDeviceContext()->addTimelineCallback(
            [device = getDeviceContext()->getDevice(), pool = myDescriptorPool](uint64_t){
                vkDestroyDescriptorPool(device, pool, nullptr);
        });
}
