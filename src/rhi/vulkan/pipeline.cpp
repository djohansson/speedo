#include <rhi/pipeline.h>
#include <rhi/rhi.h>
#include <rhi/vulkan/convert.h>
#include <rhi/vulkan/utils.h>

#include <core/file.h>

#include <filesystem>
#include <format>
#include <iostream>

namespace rhi
{

IMPLEMENT_OBJECT_GETINSTANCE(PipelineLayout<kVk>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(PipelineLayout<kVk>);
IMPLEMENT_OBJECT_GETINSTANCE(Pipeline<kVk>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(Pipeline<kVk>);

#pragma pack(push, 1)
template <>
struct PipelineCacheHeader<kVk>
{
	uint32_t headerLength = 0UL;
	uint32_t cacheHeaderVersion = 0UL;
	uint32_t vendorID = 0UL;
	uint32_t deviceID = 0UL;
	std::array<uint8_t, VK_UUID_SIZE> pipelineCacheUUID;
};
#pragma pack(pop)

namespace pipeline
{

using namespace core::file;

bool IsCacheValid(
	const PipelineCacheHeader<kVk>& header,
	const PhysicalDeviceProperties<kVk>& physicalDeviceProperties)
{
	return (
		header.headerLength > 0 &&
		header.cacheHeaderVersion == VK_PIPELINE_CACHE_HEADER_VERSION_ONE &&
		header.vendorID == physicalDeviceProperties.properties.vendorID &&
		header.deviceID == physicalDeviceProperties.properties.deviceID &&
		memcmp(
			header.pipelineCacheUUID.data(),
			physicalDeviceProperties.properties.pipelineCacheUUID,
			std::size(header.pipelineCacheUUID)) == 0);
}

PipelineCacheHandle<kVk> LoadPipelineCache(const std::filesystem::path& cacheFilePath, Device<kVk>& device, const Instance<kVk>& instance)
{
	std::vector<char> cacheData;

	auto loadCacheOp = [&instance, &device, &cacheData](auto& inStream) -> std::error_code
	{
		if (auto result = inStream(cacheData); failure(result))
			return std::make_error_code(result);

		const auto* header = reinterpret_cast<const PipelineCacheHeader<kVk>*>(cacheData.data());

		if (cacheData.empty() ||
			!IsCacheValid(*header, instance.GetPhysicalDeviceInfo(device.GetPhysicalDevice()).deviceProperties))
		{
			std::cerr << "Invalid pipeline cache, creating new." << '\n';
			cacheData.clear();
		}

		return {};
	};

	if (auto fileInfo = GetRecord<false>(cacheFilePath); fileInfo)
		fileInfo = LoadBinary<false>(cacheFilePath, loadCacheOp);

	VkPipelineCacheCreateInfo createInfo{.sType=VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
	createInfo.initialDataSize = cacheData.size();
	createInfo.pInitialData =
		(static_cast<unsigned int>(!cacheData.empty()) != 0U) ? cacheData.data() : nullptr;

	PipelineCacheHandle<kVk> cache;
	VK_CHECK(vkCreatePipelineCache(
		device,
		&createInfo,
		&instance.GetHostAllocationCallbacks(),
		&cache));

	return cache;
}

std::vector<std::byte>
GetPipelineCacheData(DeviceHandle<kVk> device, PipelineCacheHandle<kVk> pipelineCache)
{
	std::vector<std::byte> cacheData;
	size_t cacheDataSize = 0;
	VK_CHECK(vkGetPipelineCacheData(device, pipelineCache, &cacheDataSize, nullptr));
	if (cacheDataSize != 0U)
	{
		cacheData.resize(cacheDataSize);
		VK_CHECK(vkGetPipelineCacheData(device, pipelineCache, &cacheDataSize, cacheData.data()));
	}

	return cacheData;
};

std::expected<Record, std::error_code> SavePipelineCache(
	const std::filesystem::path& cacheFilePath,
	DeviceHandle<kVk> device,
	PhysicalDeviceProperties<kVk> physicalDeviceProperties,
	PipelineCacheHandle<kVk> pipelineCache)
{
	// todo: move to gfx-vulkan.cpp
	auto saveCacheOp =
		[&device, &pipelineCache, &physicalDeviceProperties](auto& out) -> std::error_code
	{
		auto cacheData = GetPipelineCacheData(device, pipelineCache);

		if (cacheData.empty())
		{
			std::println("Failed to get pipeline cache.");
		
			return std::make_error_code(std::errc::invalid_argument);
		}

		const auto* header = reinterpret_cast<const PipelineCacheHeader<kVk>*>(cacheData.data());

		if (!IsCacheValid(*header, physicalDeviceProperties))
		{
			std::println("Invalid pipeline cache, will not save.");

			return std::make_error_code(std::errc::invalid_argument);
		}

		if (auto result = out(cacheData); failure(result))
			return std::make_error_code(result);

		return {}; // success
	};

	return SaveBinary<true>(cacheFilePath, saveCacheOp);
}

} // namespace pipeline

template <>
void PipelineLayout<kVk>::Swap(PipelineLayout& rhs) noexcept
{
	DeviceObject<PipelineLayout<kVk>>::Swap(rhs);
	std::swap(myShaderModules, rhs.myShaderModules);
	std::swap(myDescriptorSetLayouts, rhs.myDescriptorSetLayouts);
	std::swap(myPushConstantRanges, rhs.myPushConstantRanges);
	std::swap(myLayout, rhs.myLayout);
}

template <>
PipelineLayout<kVk>& PipelineLayout<kVk>::operator=(PipelineLayout<kVk>&& other) noexcept
{
	Swap(other);
	return *this;
}

template <>
PipelineLayout<kVk>::PipelineLayout(PipelineLayout<kVk>&& other) noexcept
{
	Swap(other);
}

template <>
PipelineLayout<kVk>::PipelineLayout(
	CreateDescType&& desc,
	std::vector<ShaderModule<kVk>>&& shaderModules,
	DescriptorSetLayoutFlatMap<kVk>&& descriptorSetLayouts,
	PipelineLayoutHandle<kVk>&& layout)
	: DeviceObject<PipelineLayout<kVk>>(std::forward<CreateDescType>(desc))
	, myShaderModules(std::exchange(shaderModules, {}))
	, myDescriptorSetLayouts(std::exchange(descriptorSetLayouts, {}))
	, myPushConstantRanges(descriptorset::GetPushConstantRanges<kVk>(myDescriptorSetLayouts))
	, myLayout(std::forward<PipelineLayoutHandle<kVk>>(layout))
{}

template <>
PipelineLayout<kVk>::PipelineLayout(
	CreateDescType&& desc,
	std::vector<ShaderModule<kVk>>&& shaderModules,
	DescriptorSetLayoutFlatMap<kVk>&& descriptorSetLayouts)
	: PipelineLayout(
		  std::forward<CreateDescType>(desc),
		  std::forward<std::vector<ShaderModule<kVk>>>(shaderModules),
		  std::forward<DescriptorSetLayoutFlatMap<kVk>>(descriptorSetLayouts),
		  // read from desc, not GetDesc(): this runs before the delegated constructor has initialized the base
		  [&descriptorSetLayouts, &desc, this]
		  {
			  // todo: rewrite flatmap so that keys and vals are stored as separate arrays so that we dont have to make this conversion
			  auto handles = descriptorset::GetDescriptorSetLayoutHandles<kVk>(descriptorSetLayouts);
			  auto pushConstantRanges =
				  descriptorset::GetPushConstantRanges<kVk>(descriptorSetLayouts);

			  VkPipelineLayoutCreateInfo pipelineLayoutInfo{
				  .sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
			  pipelineLayoutInfo.setLayoutCount = handles.size();
			  pipelineLayoutInfo.pSetLayouts = handles.data();
			  pipelineLayoutInfo.pushConstantRangeCount = pushConstantRanges.size();
			  pipelineLayoutInfo.pPushConstantRanges = pushConstantRanges.data();

			  VkPipelineLayout layout;
			  VK_CHECK(vkCreatePipelineLayout(
				  desc.device,
				  &pipelineLayoutInfo,
				  &GetInstance().GetHostAllocationCallbacks(),
				  &layout));

			  Track(desc.device, VK_OBJECT_TYPE_PIPELINE_LAYOUT, layout, GetDebugName(desc));

			  return layout;
		  }())
{}

namespace pipeline
{

// a shader set layout as vulkan takes it: every binding partially bound, so the shaders may leave elements unwritten
static DescriptorSetLayoutCreateDesc<kVk> ToDescriptorSetLayoutCreateDesc(const ShaderSetLayout& layout)
{
	DescriptorSetLayoutCreateDesc<kVk> desc;
	for (const auto& binding : layout.bindings)
	{
		desc.bindings.push_back(DescriptorSetLayoutBinding<kVk>{
			.binding = binding.binding,
			.descriptorType = vk::ToVk(binding.type),
			.descriptorCount = binding.count,
			.stageFlags = vk::ToVk(binding.stages),
			.pImmutableSamplers = nullptr});
		desc.bindingFlags.push_back(VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT);
		desc.variableNames.push_back(binding.name);
		desc.variableNameHashes.push_back(binding.nameHash);
	}
	if (const auto& pushConstants = layout.pushConstants)
		desc.pushConstantRange =
			PushConstantRange<kVk>{vk::ToVk(pushConstants->stages), pushConstants->offset, pushConstants->size};
	return desc;
}

} // namespace pipeline

template <>
PipelineLayout<kVk>::PipelineLayout(
	CreateDescType&& desc,
	const ShaderSet& shaderSet)
	: PipelineLayout(
		std::forward<CreateDescType>(desc),
		// read from desc, not GetDesc(): this runs before the delegated constructor has initialized the base
		[&shaderSet, &desc, this]
		{
			std::vector<ShaderModule<kVk>> shaderModules;
			shaderModules.reserve(shaderSet.shaders.size());
			for (auto shader : shaderSet.shaders)
				shaderModules.emplace_back(
					ShaderModuleCreateDesc<kVk>{
						SuperType::CreateDeviceObjectCreateDesc("ShaderModule", desc.device),
						shader,
					});

			return shaderModules;
		}(),
		[&shaderSet, &desc]
		{
			DescriptorSetLayoutFlatMap<kVk> map;
			for (const auto& [set, shaderSetLayout] : shaderSet.layouts)
			{
				auto layout = pipeline::ToDescriptorSetLayoutCreateDesc(shaderSetLayout);
				layout.instance = desc.instance;
				layout.device = desc.device;
				layout.uuid = uuids::NewUuid();
				layout.name = std::format("{} DescriptorSetLayout {}", GetDebugName(desc), set);
				map.emplace(set, DescriptorSetLayout<kVk>(std::move(layout)));
			}

			return map;
		}())
{}

template <>
PipelineLayout<kVk>::~PipelineLayout()
{
	if (IsValid())
	{
		Untrack(VK_OBJECT_TYPE_PIPELINE_LAYOUT, myLayout);
		vkDestroyPipelineLayout(
			GetDevice(),
			myLayout,
			&GetInstance().GetHostAllocationCallbacks());
	}
}

template <>
uint64_t Pipeline<kVk>::InternalCalculateHashKey(GraphicsPipelineVariant variant) const
{
	ZoneScopedN("Pipeline::InternalCalculateHashKey");

	thread_local std::unique_ptr<XXH3_state_t, XXH_errorcode (*)(XXH3_state_t*)> gThreadXxhState{
		XXH3_createState(), XXH3_freeState};

	auto result = XXH3_64bits_reset(gThreadXxhState.get());
	ENSURE(result != XXH_ERROR);

	result = XXH3_64bits_update(gThreadXxhState.get(), &myBindPoint, sizeof(myBindPoint));
	ENSURE(result != XXH_ERROR);

	auto layoutIt = InternalGetLayout();
	ENSURE(layoutIt != myPipelineLayouts.end());
	auto* layoutHandle = static_cast<PipelineLayoutHandle<kVk>>(*layoutIt);
	result = XXH3_64bits_update(gThreadXxhState.get(), &layoutHandle, sizeof(layoutHandle));
	//result = XXH3_64bits_update(gThreadXxhState.get(), &(*layoutIt), sizeof(*layoutIt));
	ENSURE(result != XXH_ERROR);

	if (myBindPoint == PipelineBindPoint::kGraphics)
	{
		std::array<uint8_t, 2> key{static_cast<uint8_t>(variant.topology), static_cast<uint8_t>(variant.blend)};
		result = XXH3_64bits_update(gThreadXxhState.get(), key.data(), key.size());
		ENSURE(result != XXH_ERROR);
	}

	// todo: hash more releveant state for the current bind point... framebuffer, model, etc.

	// todo: rendertargets need to use hash key derived from its state and not its handles/pointers, since they are recreated often
	// auto [renderPassHandle, frameBufferHandle] =
	//     static_cast<RenderTarget<kVk>::ValueType>(*GetRenderTarget());
	// result = XXH3_64bits_update(threadXXHState.get(), &renderPassHandle, sizeof(renderPassHandle));
	// result = XXH3_64bits_update(threadXXHState.get(), &frameBufferHandle, sizeof(frameBufferHandle));
	// ASSERT(result != XXH_ERROR);

	return XXH3_64bits_digest(gThreadXxhState.get());
}

template <>
void Pipeline<kVk>::InternalPrepareDescriptorSets()
{
	const auto layoutIt = InternalGetLayout();
	ENSURE(layoutIt != myPipelineLayouts.end());
	const auto& layout = *layoutIt;

	for (const auto& [set, setLayout] : layout.GetDescriptorSetLayouts())
	{
		auto setLayoutHandle = static_cast<DescriptorSetLayoutHandle<kVk>>(setLayout);
		auto setStateIt = myDescriptorMap.find(setLayoutHandle);
		if (setStateIt == myDescriptorMap.end())
		{
			auto insertResultPair = myDescriptorMap.emplace(
				setLayoutHandle,
				std::make_tuple(
					core::UpgradableSharedMutex{},
					DescriptorSetStatus::kReady,
					BindingsMap<kVk>{},
					BindingsData<kVk>{},
					DescriptorUpdateTemplate<kVk>{
						DescriptorUpdateTemplateCreateDesc<kVk>{
							GetDevice().CreateDeviceObjectCreateDesc("DescriptorUpdateTemplate"),
							((setLayout.GetDesc().flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR) != 0U)
								? VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR
								: VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET,
							static_cast<VkDescriptorSetLayout>(setLayout),
							myBindPoint,
							static_cast<VkPipelineLayout>(layout),
							set}},
					((setLayout.GetDesc().flags &
					VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR) != 0U)
						? std::nullopt
						: std::make_optional(DescriptorSetArrayList<kVk>{})));
			auto& [insertIt, insertResult] = insertResultPair;
			ENSURE(insertResult);
			ENSURE(insertIt != myDescriptorMap.end());
			auto& [bindingIndex, bindingTuple] = *insertIt;
			auto& [mutex, setState, bindingsMap, bindingsData, setTemplate, setOptionalArrayList] = bindingTuple;

			if (setOptionalArrayList)
			{
				auto& setArrayList = setOptionalArrayList.value();

				setArrayList.emplace_front(
					DescriptorSetArray<kVk>(
						DescriptorSetArrayCreateDesc<kVk>{
							GetDevice().CreateDeviceObjectCreateDesc("DescriptorSetArray"),
							myDescriptorPool
						},
						setLayout),
					0);
			}
		}
	}
}

template <>
void Pipeline<kVk>::InternalResetGraphicsState()
{
	myGraphicsState.shaderStages.clear();
	myGraphicsState.shaderStageFlags = {};

	myGraphicsState.vertexInput = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
		.pNext = nullptr,
		.flags = 0,
		.vertexBindingDescriptionCount = 0,
		.pVertexBindingDescriptions = nullptr,
		.vertexAttributeDescriptionCount = 0,
		.pVertexAttributeDescriptions = nullptr};

	myGraphicsState.inputAssembly = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.pNext = nullptr,
		.flags = 0,
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
		.primitiveRestartEnable = VK_FALSE};

	myGraphicsState.viewports.clear();
	myGraphicsState.viewports.emplace_back(VkViewport{.x=0.0F, .y=0.0F, .width=0, .height=0, .minDepth=0.0F, .maxDepth=1.0F});

	myGraphicsState.scissorRects.clear();
	myGraphicsState.scissorRects.emplace_back(VkRect2D{.offset={.x=0, .y=0}, .extent={.width=0, .height=0}});

	myGraphicsState.viewport = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.pNext = nullptr,
		.flags = 0,
		.viewportCount = static_cast<uint32_t>(myGraphicsState.viewports.size()),
		.pViewports = myGraphicsState.viewports.data(),
		.scissorCount = static_cast<uint32_t>(myGraphicsState.scissorRects.size()),
		.pScissors = myGraphicsState.scissorRects.data()};

	myGraphicsState.rasterization = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.pNext = nullptr,
		.flags = 0,
		.depthClampEnable = VK_FALSE,
		.rasterizerDiscardEnable = VK_FALSE,
		.polygonMode = VK_POLYGON_MODE_FILL,
		.cullMode = VK_CULL_MODE_BACK_BIT,
		.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
		.depthBiasEnable = VK_FALSE,
		.depthBiasConstantFactor = 0.0F,
		.depthBiasClamp = 0.0F,
		.depthBiasSlopeFactor = 0.0F,
		.lineWidth = 1.0F};

	myGraphicsState.multisample = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.pNext = nullptr,
		.flags = 0,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
		.sampleShadingEnable = VK_FALSE,
		.minSampleShading = 1.0F,
		.pSampleMask = nullptr,
		.alphaToCoverageEnable = VK_FALSE,
		.alphaToOneEnable = VK_FALSE};

	myGraphicsState.depthStencil = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		.pNext = nullptr,
		.flags = 0,
		.depthTestEnable = VK_TRUE,
		.depthWriteEnable = VK_TRUE,
		.depthCompareOp = VK_COMPARE_OP_LESS,
		.depthBoundsTestEnable = VK_FALSE,
		.stencilTestEnable = VK_FALSE,
		.front = {},
		.back = {},
		.minDepthBounds = 0.0F,
		.maxDepthBounds = 1.0F};

	myGraphicsState.colorBlendAttachments.clear();
	myGraphicsState.colorBlendAttachments.emplace_back(PipelineColorBlendAttachmentState<kVk>{
		.blendEnable = VK_FALSE,
		.srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
		.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO,
		.colorBlendOp = VK_BLEND_OP_ADD,
		.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
		.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
		.alphaBlendOp = VK_BLEND_OP_ADD,
		.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
			VK_COLOR_COMPONENT_A_BIT});

	myGraphicsState.colorBlend = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.pNext = nullptr,
		.flags = 0,
		.logicOpEnable = VK_FALSE,
		.logicOp = VK_LOGIC_OP_COPY,
		.attachmentCount = static_cast<uint32_t>(myGraphicsState.colorBlendAttachments.size()),
		.pAttachments = myGraphicsState.colorBlendAttachments.data(),
		.blendConstants = {0.0F, 0.0F, 0.0F, 0.0F}};

	myGraphicsState.dynamicStateDescs.clear();
	myGraphicsState.dynamicStateDescs.emplace_back(VK_DYNAMIC_STATE_VIEWPORT);
	myGraphicsState.dynamicStateDescs.emplace_back(VK_DYNAMIC_STATE_SCISSOR);
	// per draw (e.g. per material, for double sided ones), see CommandEncoder::SetCullMode
	myGraphicsState.dynamicStateDescs.emplace_back(VK_DYNAMIC_STATE_CULL_MODE_EXT);
	myGraphicsState.dynamicStateDescs.emplace_back(VK_DYNAMIC_STATE_FRONT_FACE_EXT);

	myGraphicsState.dynamicState = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
		.pNext = nullptr,
		.flags = 0,
		.dynamicStateCount = static_cast<uint32_t>(myGraphicsState.dynamicStateDescs.size()),
		.pDynamicStates = myGraphicsState.dynamicStateDescs.data()};
}

template <>
void Pipeline<kVk>::InternalResetComputeState()
{
	//myComputeState....
}

template <>
void Pipeline<kVk>::InternalResetDescriptorPool()
{
	vkResetDescriptorPool(GetDevice(), myDescriptorPool, 0);
}

template <>
PipelineHandle<kVk> Pipeline<kVk>::InternalCreateGraphicsPipeline(uint64_t hashKey, GraphicsPipelineVariant variant)
{
	ZoneScopedN("Pipeline::InternalCreateGraphicsPipeline");

	const auto layoutIt = InternalGetLayout();
	ENSURE(layoutIt != myPipelineLayouts.end());
	const auto& layout = *layoutIt;

	VkGraphicsPipelineCreateInfo pipelineInfo{.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
	pipelineInfo.pNext = myGraphicsState.dynamicRendering.has_value() ? &myGraphicsState.dynamicRendering.value() : nullptr;
	pipelineInfo.flags = 0;
	pipelineInfo.stageCount = static_cast<uint32_t>(myGraphicsState.shaderStages.size());
	pipelineInfo.pStages = myGraphicsState.shaderStages.data();
	pipelineInfo.pVertexInputState = &myGraphicsState.vertexInput;
	auto inputAssembly = myGraphicsState.inputAssembly;
	inputAssembly.topology = vk::ToVk(variant.topology);
	pipelineInfo.pInputAssemblyState = &inputAssembly;
	pipelineInfo.pViewportState = &myGraphicsState.viewport;
	pipelineInfo.pRasterizationState = &myGraphicsState.rasterization;
	pipelineInfo.pMultisampleState = &myGraphicsState.multisample;
	auto depthStencil = myGraphicsState.depthStencil;
	auto colorBlendAttachments = myGraphicsState.colorBlendAttachments;
	auto colorBlend = myGraphicsState.colorBlend;
	if (variant.blend == BlendMode::kAlpha)
	{
		depthStencil.depthWriteEnable = VK_FALSE;
		for (auto& attachment : colorBlendAttachments)
		{
			attachment.blendEnable = VK_TRUE;
			attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
			attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
			attachment.colorBlendOp = VK_BLEND_OP_ADD;
			attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
			attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
			attachment.alphaBlendOp = VK_BLEND_OP_ADD;
		}
		colorBlend.pAttachments = colorBlendAttachments.data();
	}
	pipelineInfo.pDepthStencilState = &depthStencil;
	pipelineInfo.pColorBlendState = &colorBlend;
	pipelineInfo.pDynamicState = &myGraphicsState.dynamicState;
	pipelineInfo.layout = layout;
	pipelineInfo.renderPass = std::get<0>(myRenderTarget);
	// render targets create exactly one subpass per render pass (and ignore it with dynamic rendering). a pipeline is
	// only compatible with the subpass it was created for, so multi-subpass passes would need the current subpass
	// index tracked in the graphics state and included in the pipeline hash, rather than a loop here.
	pipelineInfo.subpass = 0;
	pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
	pipelineInfo.basePipelineIndex = -1;

	VkPipeline pipelineHandle;
	VK_CHECK(vkCreateGraphicsPipelines(
		GetDevice(),
		myCache,
		1,
		&pipelineInfo,
		&GetInstance().GetHostAllocationCallbacks(),
		&pipelineHandle));

	Track(GetDevice(), VK_OBJECT_TYPE_PIPELINE, pipelineHandle, std::format("{} Graphics Pipeline {}", GetName(), hashKey));

	return pipelineHandle;
}

template <>
PipelineHandle<kVk> Pipeline<kVk>::InternalCreateComputePipeline(uint64_t hashKey)
{
	ZoneScopedN("Pipeline::InternalCreateComputePipeline");

	const auto layoutIt = InternalGetLayout();
	ENSURE(layoutIt != myPipelineLayouts.end());
	const auto& layout = *layoutIt;

	VkComputePipelineCreateInfo pipelineInfo{.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .pNext=nullptr, .flags=0};
	pipelineInfo.stage = myComputeState.shaderStage;
	pipelineInfo.layout = layout;
	pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
	pipelineInfo.basePipelineIndex = -1;

	VkPipeline pipelineHandle;
	VK_CHECK(vkCreateComputePipelines(
		GetDevice(),
		myCache,
		1,
		&pipelineInfo,
		&GetInstance().GetHostAllocationCallbacks(),
		&pipelineHandle));

	Track(GetDevice(), VK_OBJECT_TYPE_PIPELINE, pipelineHandle, std::format("{} Compute Pipeline {}", GetName(), hashKey));

	return pipelineHandle;
}

template <>
PipelineHandle<kVk> Pipeline<kVk>::InternalGetPipeline(GraphicsPipelineVariant variant)
{
	ZoneScopedN("Pipeline::InternalGetPipeline");

	auto [keyValIt, insertResult] =
		myPipelineMap.insert({InternalCalculateHashKey(variant), PipelineHandle<kVk>{}});
	auto& [key, pipelineHandle] = *keyValIt;
	auto pipelineHandleAtomic = std::atomic_ref(pipelineHandle);

	if (insertResult)
	{
		ZoneScopedN("Pipeline::InternalGetPipeline::store");

		switch (myBindPoint)
		{
		case PipelineBindPoint::kGraphics:
			pipelineHandleAtomic.store(InternalCreateGraphicsPipeline(key, variant), std::memory_order_release);
			break;
		case PipelineBindPoint::kCompute:
			pipelineHandleAtomic.store(InternalCreateComputePipeline(key), std::memory_order_release);
			break;
		default:
			ASSERTF(false, "Not implemented");
		}

		pipelineHandleAtomic.notify_all();
	}
	else
	{
		ZoneScopedN("Pipeline::InternalGetPipeline::wait");

		pipelineHandleAtomic.wait(nullptr, std::memory_order_acquire);
	}

	return pipelineHandleAtomic;
}

template <>
void Pipeline<kVk>::BindPipeline(
	CommandBufferHandle<kVk> cmd, PipelineBindPoint bindPoint, PipelineHandle<kVk> handle) const
{
	ZoneScopedN("Pipeline::BindPipeline");

	vkCmdBindPipeline(cmd, vk::ToVk(bindPoint), handle);
}

template <>
PipelineHandle<kVk> Pipeline<kVk>::BindPipelineAuto(CommandBufferHandle<kVk> cmd, GraphicsPipelineVariant variant)
{
	auto* handle = InternalGetPipeline(variant);

	BindPipeline(cmd, myBindPoint, handle);
	
	return handle;
}

template <>
PipelineLayoutHandle<kVk> Pipeline<kVk>::GetLayout() const noexcept
{
	const auto layoutIt = InternalGetLayout();
	
	if (layoutIt == myPipelineLayouts.end())
		return VK_NULL_HANDLE;

	return static_cast<PipelineLayoutHandle<kVk>>(*layoutIt);
}

template <>
void Pipeline<kVk>::PushConstants(CommandBufferHandle<kVk> cmd, std::span<const std::byte> data, uint32_t offset) const
{
	const auto layoutIt = InternalGetLayout();
	ENSURE(layoutIt != myPipelineLayouts.end());

	// vkCmdPushConstants requires the stage flags of every range that overlaps the pushed bytes, and only those
	const auto size = static_cast<uint32_t>(data.size());
	ShaderStageFlags<kVk> stageFlags = 0;
	for (const auto& range : layoutIt->GetPushConstantRanges())
		if (range.offset < offset + size && offset < range.offset + range.size)
			stageFlags |= range.stageFlags;

	ENSUREF(stageFlags != 0, "No push constant range in the current layout covers the pushed data.");

	vkCmdPushConstants(cmd, *layoutIt, stageFlags, offset, size, data.data());
}

template <>
PipelineLayoutHandle<kVk> Pipeline<kVk>::CreateLayout(const ShaderSet& shaderSet)
{
	const auto& [layoutIt, wasInserted] = myPipelineLayouts.emplace(
		PipelineLayout<kVk>(
			PipelineLayoutCreateDesc<kVk>{
				SuperType::CreateDeviceObjectCreateDesc("PipelineLayout"),
			},
			shaderSet));

	return static_cast<PipelineLayoutHandle<kVk>>(*layoutIt);
}

template <>
void Pipeline<kVk>::BindLayoutAuto(PipelineLayoutHandle<kVk> layoutHandle, PipelineBindPoint bindPoint)
{
	myBindPoint = bindPoint;
	myCurrentLayoutIt = myPipelineLayouts.find(layoutHandle);
	ENSURE(myCurrentLayoutIt != myPipelineLayouts.end());
	const auto& layout = *myCurrentLayoutIt;
	const auto& shaderModules = layout.GetShaderModules();

	ENSURE(!shaderModules.empty());

	switch (myBindPoint)
	{
	case PipelineBindPoint::kGraphics:
		myGraphicsState.shaderStageFlags = {};
		myGraphicsState.shaderStages.clear();
		myGraphicsState.shaderStages.reserve(shaderModules.size());
		for (const auto& shader : shaderModules)
		{
			const auto& [entryPointName, shaderStage, launchParams] = shader.GetEntryPoint();

			if (Any(shaderStage & ShaderStage::kAllGraphics))
			{
				myGraphicsState.shaderStages.emplace_back(PipelineShaderStageCreateInfo<kVk>{
					.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
					.pNext=nullptr,
					.flags=0,
					.stage=static_cast<VkShaderStageFlagBits>(vk::ToVk(shaderStage)),
					.module=shader,
					.pName=entryPointName.c_str(),
					.pSpecializationInfo=nullptr});

				myGraphicsState.shaderStageFlags |= vk::ToVk(shaderStage);
			}
		}
		break;
	case PipelineBindPoint::kCompute:
		{
			// todo: better handling of multiple compute shaders
			const auto& [entryPointName, shaderStage, launchParams] = shaderModules.back().GetEntryPoint();
			ENSURE(shaderStage == ShaderStage::kCompute);
			myComputeState.shaderStage = {
				.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
				.pNext = nullptr,
				.flags = 0,
				.stage = VK_SHADER_STAGE_COMPUTE_BIT,
				.module = shaderModules.back(),
				.pName = entryPointName.c_str(),
				.pSpecializationInfo = nullptr};
			myComputeState.launchParameters = launchParams.value_or(ComputeLaunchParameters{});
		}
		break;
	default:
		ASSERTF(false, "Not implemented");
		break;
	};

	InternalPrepareDescriptorSets();
}

template <>
void Pipeline<kVk>::SetRenderTarget(IRenderTarget<kVk>& renderTarget)
{
	auto extent = renderTarget.GetExtent();

	myGraphicsState.viewports[0].width = static_cast<float>(extent.width);
	myGraphicsState.viewports[0].height = static_cast<float>(extent.height);
	myGraphicsState.scissorRects[0].offset = {.x = 0, .y = 0};
	myGraphicsState.scissorRects[0].extent = {.width = extent.width, .height = extent.height};
	myGraphicsState.dynamicRendering = renderTarget.GetPipelineRenderingCreateInfo();
	
	myRenderTarget = renderTarget.GetHandle();
}

template <>
void Pipeline<kVk>::InternalUpdateDescriptorSet(
	const DescriptorSetLayout<kVk>& setLayout,
	const BindingsData<kVk>& bindingsData,
	const DescriptorUpdateTemplate<kVk>& setTemplate,
	DescriptorSetArrayList<kVk>& setArrayList) const
{
	ZoneScopedN("Pipeline::InternalUpdateDescriptorSet");

	bool setArrayListIsEmpty = setArrayList.empty();
	bool frontArrayIsFull = setArrayListIsEmpty
								? false
								: std::get<1>(setArrayList.front()) ==
									  (DescriptorSetArray<kVk>::Capacity() - 1);

	if (setArrayListIsEmpty || frontArrayIsFull)
	{
		setArrayList.emplace_front(std::make_tuple(
			DescriptorSetArray<kVk>(
				DescriptorSetArrayCreateDesc<kVk>{GetDevice().CreateDeviceObjectCreateDesc("DescriptorSetArray"), myDescriptorPool}, setLayout),
			~0));
	}

	auto& [setArray, setIndex] = setArrayList.front();
	++setIndex;
	ENSURE(setIndex < setArray.Capacity());
	auto* setHandle = setArray[setIndex];

	{
		ZoneScopedN(
			"Pipeline::InternalUpdateDescriptorSet::vkUpdateDescriptorSetWithTemplate");

		vkUpdateDescriptorSetWithTemplate(
			GetDevice(), setHandle, setTemplate, bindingsData.data());
	}
}

template <>
void Pipeline<kVk>::InternalPushDescriptorSet(
	CommandBufferHandle<kVk> cmd,
	PipelineLayoutHandle<kVk> layout,
	const BindingsData<kVk>& bindingsData,
	const DescriptorUpdateTemplate<kVk>& setTemplate)
{
	ZoneScopedN("Pipeline::InternalPushDescriptorSet");

	gVkCmdPushDescriptorSetWithTemplateKHR(
		cmd, setTemplate, layout, setTemplate.GetDesc().set, bindingsData.data());
}

template <>
void Pipeline<kVk>::InternalUpdateDescriptorSetTemplate(
	const BindingsMap<kVk>& bindingsMap, DescriptorUpdateTemplate<kVk>& setTemplate)
{
	ZoneScopedN("Pipeline::InternalUpdateDescriptorSetTemplate");

	std::vector<DescriptorUpdateTemplateEntry<kVk>> entries;
	entries.reserve(bindingsMap.size());

	for (const auto& [index, binding] : bindingsMap)
	{
		const auto& [offset, count, type, ranges] = binding;

		uint32_t rangeOffset = 0UL;

		for (const auto& [low, high] : ranges)
		{
			auto rangeCount = high - low;

			entries.emplace_back(DescriptorUpdateTemplateEntry<kVk>{
				.dstBinding=index,
				.dstArrayElement=low,
				.descriptorCount=rangeCount,
				.descriptorType=type,
				.offset=(offset + rangeOffset) * sizeof(BindingVariant<kVk>),
				.stride=sizeof(BindingVariant<kVk>)});

			rangeOffset += rangeCount;
		}
	}

	setTemplate.SetEntries(std::move(entries));
}

template <>
void Pipeline<kVk>::BindDescriptorSet(
	CommandBufferHandle<kVk> cmd,
	DescriptorSetHandle<kVk> handle,
	PipelineBindPoint bindPoint,
	PipelineLayoutHandle<kVk> layoutHandle,
	uint32_t set,
	std::optional<uint32_t> bufferOffset) const
{
	ZoneScopedN("Pipeline::BindDescriptorSet");

	vkCmdBindDescriptorSets(
		cmd,
		vk::ToVk(bindPoint),
		layoutHandle,
		set,
		1,
		&handle,
		bufferOffset ? 1 : 0,
		bufferOffset ? &bufferOffset.value() : nullptr);
}

template <>
void Pipeline<kVk>::BindDescriptorSetAuto(
	CommandBufferHandle<kVk> cmd,
	uint32_t set,
	std::optional<uint32_t> bufferOffset)
{
	ZoneScopedN("Pipeline::BindDescriptorSetAuto");

	const auto layoutIt = InternalGetLayout();
	ENSURE(layoutIt != myPipelineLayouts.end());
	const auto& layout = *layoutIt;
	const auto& setLayout = layout.GetDescriptorSetLayout(set);
	auto& [mutex, setState, bindingsMap, bindingsData, setTemplate, setOptionalArrayList] = myDescriptorMap.at(setLayout);
	
	if (setOptionalArrayList)
	{
		mutex.lock_upgrade();

		auto dirtyState = DescriptorSetStatus::kDirty;
		if (std::atomic_ref(setState).compare_exchange_weak(dirtyState, DescriptorSetStatus::kReady, std::memory_order_acq_rel))
		{
			mutex.unlock_upgrade_and_lock();

			InternalUpdateDescriptorSet(
				setLayout, bindingsData, setTemplate, setOptionalArrayList.value());

			mutex.unlock_and_lock_shared();
		}
		else [[likely]]
		{
			mutex.unlock_upgrade_and_lock_shared();
		}

		auto& setArrayList = setOptionalArrayList.value();
		ENSURE(!setArrayList.empty());
		auto& [setArray, setIndex] = setArrayList.front();
		auto* handle = setArray[setIndex];

		BindDescriptorSet(
			cmd,
			handle,
			myBindPoint,
			static_cast<PipelineLayoutHandle<kVk>>(layout),
			set,
			bufferOffset);
	}
	else if (gVkCmdPushDescriptorSetWithTemplateKHR != nullptr)
	{
		mutex.lock_shared();

		InternalPushDescriptorSet(
			cmd,
			static_cast<PipelineLayoutHandle<kVk>>(layout),
			bindingsData,
			setTemplate);
	}
	else 
	{
		ENSUREF(false, "Push descriptor set not supported");
	}

	mutex.unlock_shared();
}

template <>
Pipeline<kVk>::Pipeline(CreateDescType&& desc)
	: DeviceObject(std::forward<CreateDescType>(desc))
	, myDescriptorPool(
		[this]
		{
			static constexpr uint32_t kMaxInlineUniformBlockBindings = 128 * 1024;
			static constexpr uint32_t kMaxSets = 16 * 1024; // per pool size

			std::vector<VkDescriptorPoolSize> poolSizes;
			for (const auto& [type, count] : GetDesc().descriptorPoolSizes)
				poolSizes.push_back({.type = vk::ToVk(type), .descriptorCount = count});
			ENSURE(!poolSizes.empty());

			VkDescriptorPoolInlineUniformBlockCreateInfo inlineUniformBlockInfo{
				.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_INLINE_UNIFORM_BLOCK_CREATE_INFO};
			inlineUniformBlockInfo.maxInlineUniformBlockBindings = kMaxInlineUniformBlockBindings;

			VkDescriptorPoolCreateInfo poolInfo{.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
			poolInfo.pNext = &inlineUniformBlockInfo;
			poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
			poolInfo.pPoolSizes = poolSizes.data();
			poolInfo.maxSets = kMaxSets * static_cast<uint32_t>(poolSizes.size());
			poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
			// VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT
			// VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT

			VkDescriptorPool outDescriptorPool;
			VK_CHECK(vkCreateDescriptorPool(
				GetDevice(),
				&poolInfo,
				&GetInstance().GetHostAllocationCallbacks(),
				&outDescriptorPool));

			Track(GetDevice(), VK_OBJECT_TYPE_DESCRIPTOR_POOL, outDescriptorPool, std::format("{} DescriptorPool", GetName()));

			return outDescriptorPool;
		}())
	, myCache(pipeline::LoadPipelineCache(GetDesc().cachePath, GetDevice(), GetInstance()))
{
	InternalResetGraphicsState();
	InternalResetComputeState();

	Track(GetDevice(), VK_OBJECT_TYPE_PIPELINE_CACHE, myCache, std::format("{} PipelineCache", GetName()));
}

template <>
void Pipeline<kVk>::Swap(Pipeline& rhs) noexcept
{
	DeviceObject<Pipeline<kVk>>::Swap(rhs);
	std::swap(myDescriptorMap, rhs.myDescriptorMap);
	std::swap(myDescriptorPool, rhs.myDescriptorPool);
	std::swap(myPipelineMap, rhs.myPipelineMap);
	std::swap(myCache, rhs.myCache);
	std::swap(myBindPoint, rhs.myBindPoint);
	std::swap(myRenderTarget, rhs.myRenderTarget);
	std::swap(myPipelineLayouts, rhs.myPipelineLayouts);
	std::swap(myCurrentLayoutIt, rhs.myCurrentLayoutIt);
	std::swap(myGraphicsState, rhs.myGraphicsState);
	std::swap(myComputeState, rhs.myComputeState);
	std::swap(myRayTracingState, rhs.myRayTracingState);
}

template <>
Pipeline<kVk>::Pipeline(Pipeline&& other) noexcept
{
	Swap(other);
}

template <>
Pipeline<kVk>& Pipeline<kVk>::operator=(Pipeline<kVk>&& other) noexcept
{
	Swap(other);
	return *this;
}

template <>
Pipeline<kVk>::~Pipeline()
{
	if (!IsValid())
		return;

	if (auto fileInfo = pipeline::SavePipelineCache(
			GetDesc().cachePath,
			GetDevice(),
			GetInstance().GetPhysicalDeviceInfo(GetDevice().GetPhysicalDevice()).deviceProperties,
			myCache);
		fileInfo)
	{
		std::cout << "Saved pipeline cache to " << fileInfo.value().path << '\n';
	}
	else
	{
		std::println("Failed to save pipeline cache, error: {}", fileInfo.error().message());
	}

	for (const auto& pipelineIt : myPipelineMap)
	{
		Untrack(VK_OBJECT_TYPE_PIPELINE, pipelineIt.second);
		vkDestroyPipeline(
			GetDevice(),
			pipelineIt.second,
			&GetInstance().GetHostAllocationCallbacks());
	}

	Untrack(VK_OBJECT_TYPE_PIPELINE_CACHE, myCache);
	vkDestroyPipelineCache(
		GetDevice(),
		myCache,
		&GetInstance().GetHostAllocationCallbacks());

	myDescriptorMap.clear();

	Untrack(VK_OBJECT_TYPE_DESCRIPTOR_POOL, myDescriptorPool);
	if (myDescriptorPool != nullptr)
		vkDestroyDescriptorPool(
			GetDevice(),
			myDescriptorPool,
			&GetInstance().GetHostAllocationCallbacks());
}

} // namespace rhi
