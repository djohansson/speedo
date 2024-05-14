#include "../pipeline.h"
#include "../rhiapplication.h"

#include "../shaders/capi.h"

#include "utils.h"

#include <format>
#include <print>

#pragma pack(push, 1)
template <>
struct PipelineCacheHeader<Vk>
{
	uint32_t headerLength = 0UL;
	uint32_t cacheHeaderVersion = 0UL;
	uint32_t vendorID = 0UL;
	uint32_t deviceID = 0UL;
	uint8_t pipelineCacheUUID[VK_UUID_SIZE]{};
};
#pragma pack(pop)

namespace pipeline
{

using namespace file;

bool IsCacheValid(
	const PipelineCacheHeader<Vk>& header,
	const PhysicalDeviceProperties<Vk>& physicalDeviceProperties)
{
	return (
		header.headerLength > 0 &&
		header.cacheHeaderVersion == VK_PIPELINE_CACHE_HEADER_VERSION_ONE &&
		header.vendorID == physicalDeviceProperties.properties.vendorID &&
		header.deviceID == physicalDeviceProperties.properties.deviceID &&
		memcmp(
			header.pipelineCacheUUID,
			physicalDeviceProperties.properties.pipelineCacheUUID,
			sizeof(header.pipelineCacheUUID)) == 0);
}

PipelineCacheHandle<Vk> LoadPipelineCache(
	const std::filesystem::path& cacheFilePath, const std::shared_ptr<Device<Vk>>& device)
{
	std::vector<char> cacheData;

	auto loadCacheOp = [&device, &cacheData](auto& in) -> std::error_code
	{
		if (auto result = in(cacheData); failure(result))
			return std::make_error_code(result);

		const auto* header = reinterpret_cast<const PipelineCacheHeader<Vk>*>(cacheData.data());

		if (cacheData.empty() ||
			!IsCacheValid(*header, device->getPhysicalDeviceInfo().deviceProperties))
		{
			std::cerr << "Invalid pipeline cache, creating new." << '\n';
			cacheData.clear();
		}

		return {};
	};

	if (auto fileInfo = getRecord<false>(cacheFilePath); fileInfo)
		fileInfo = LoadBinary<false>(cacheFilePath, loadCacheOp);

	VkPipelineCacheCreateInfo createInfo{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
	createInfo.initialDataSize = cacheData.size();
	createInfo.pInitialData = (!cacheData.empty() != 0u) ? cacheData.data() : nullptr;

	PipelineCacheHandle<Vk> cache;
	VK_CHECK(vkCreatePipelineCache(
		*device,
		&createInfo,
		&device->getInstance()->getHostAllocationCallbacks(),
		&cache));

	return cache;
}

std::vector<char>
GetPipelineCacheData(DeviceHandle<Vk> device, PipelineCacheHandle<Vk> pipelineCache)
{
	std::vector<char> cacheData;
	size_t cacheDataSize = 0;
	VK_CHECK(vkGetPipelineCacheData(device, pipelineCache, &cacheDataSize, nullptr));
	if (cacheDataSize != 0u)
	{
		cacheData.resize(cacheDataSize);
		VK_CHECK(vkGetPipelineCacheData(device, pipelineCache, &cacheDataSize, cacheData.data()));
	}

	return cacheData;
};

std::expected<Record, std::error_code> SavePipelineCache(
	const std::filesystem::path& cacheFilePath,
	DeviceHandle<Vk> device,
	PhysicalDeviceProperties<Vk> physicalDeviceProperties,
	PipelineCacheHandle<Vk> pipelineCache)
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

		const auto* header = reinterpret_cast<const PipelineCacheHeader<Vk>*>(cacheData.data());

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

static PFN_vkCmdPushDescriptorSetWithTemplateKHR gVkCmdPushDescriptorSetWithTemplateKHR{};

} // namespace pipeline

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
{}

template <>
PipelineLayout<Vk>::PipelineLayout(
	const std::shared_ptr<Device<Vk>>& device,
	std::vector<ShaderModule<Vk>>&& shaderModules,
	DescriptorSetLayoutFlatMap<Vk>&& descriptorSetLayouts,
	PipelineLayoutHandle<Vk>&& layout)
	: DeviceObject(
		  device,
		  {"_PipelineLayout"},
		  1,
		  VK_OBJECT_TYPE_PIPELINE_LAYOUT,
		  reinterpret_cast<uint64_t*>(&layout))
	, myShaderModules(std::exchange(shaderModules, {}))
	, myDescriptorSetLayouts(std::exchange(descriptorSetLayouts, {}))
	, myLayout(std::forward<PipelineLayoutHandle<Vk>>(layout))
{}

template <>
PipelineLayout<Vk>::PipelineLayout(
	const std::shared_ptr<Device<Vk>>& device,
	std::vector<ShaderModule<Vk>>&& shaderModules,
	DescriptorSetLayoutFlatMap<Vk>&& descriptorSetLayouts)
	: PipelineLayout(
		  device,
		  std::forward<std::vector<ShaderModule<Vk>>>(shaderModules),
		  std::forward<DescriptorSetLayoutFlatMap<Vk>>(descriptorSetLayouts),
		  [&descriptorSetLayouts, &device]
		  {
			  // todo: rewrite flatmap so that keys and vals are stored as separate arrays so that we dont have to make this conversion
			  auto handles = descriptorset::getDescriptorSetLayoutHandles<Vk>(descriptorSetLayouts);
			  auto pushConstantRanges =
				  descriptorset::getPushConstantRanges<Vk>(descriptorSetLayouts);

			  VkPipelineLayoutCreateInfo pipelineLayoutInfo{
				  VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
			  pipelineLayoutInfo.setLayoutCount = handles.size();
			  pipelineLayoutInfo.pSetLayouts = handles.data();
			  pipelineLayoutInfo.pushConstantRangeCount = pushConstantRanges.size();
			  pipelineLayoutInfo.pPushConstantRanges = pushConstantRanges.data();

			  VkPipelineLayout layout;
			  VK_CHECK(vkCreatePipelineLayout(
				  *device,
				  &pipelineLayoutInfo,
				  &device->getInstance()->getHostAllocationCallbacks(),
				  &layout));

			  return layout;
		  }())
{}

template <>
PipelineLayout<Vk>::PipelineLayout(
	const std::shared_ptr<Device<Vk>>& device, const ShaderSet<Vk>& shaderSet)
	: PipelineLayout(
		  device,
		  [&shaderSet, &device]
		  {
			  std::vector<ShaderModule<Vk>> shaderModules;
			  shaderModules.reserve(shaderSet.shaders.size());
			  for (const auto& shader : shaderSet.shaders)
				  shaderModules.emplace_back(device, shader);
			  return shaderModules;
		  }(),
		  [&shaderSet, &device]
		  {
			  DescriptorSetLayoutFlatMap<Vk> map;
			  for (auto [set, layout] : shaderSet.layouts)
				  map.emplace(set, DescriptorSetLayout<Vk>(device, std::move(layout)));
			  return map;
		  }())
{}

template <>
PipelineLayout<Vk>::~PipelineLayout()
{
	if (myLayout != nullptr)
		vkDestroyPipelineLayout(
			*getDevice(),
			myLayout,
			&getDevice()->getInstance()->getHostAllocationCallbacks());
}

template <>
void PipelineLayout<Vk>::Swap(PipelineLayout& rhs) noexcept
{
	DeviceObject::Swap(rhs);
	std::swap(myShaderModules, rhs.myShaderModules);
	std::swap(myDescriptorSetLayouts, rhs.myDescriptorSetLayouts);
	std::swap(myLayout, rhs.myLayout);
}

template <>
const PipelineLayout<Vk>& Pipeline<Vk>::InternalGetLayout()
{
	return myGraphicsState.layouts[myLayout];
}

template <>
uint64_t Pipeline<Vk>::InternalCalculateHashKey() const
{
	ZoneScopedN("Pipeline::InternalCalculateHashKey");

	thread_local std::unique_ptr<XXH3_state_t, XXH_errorcode (*)(XXH3_state_t*)> gthreadXxhState{
		XXH3_createState(), XXH3_freeState};

	auto result = XXH3_64bits_reset(gthreadXxhState.get());
	(void)result;
	ASSERT(result != XXH_ERROR);

	result = XXH3_64bits_update(gthreadXxhState.get(), &myBindPoint, sizeof(myBindPoint));
	ASSERT(result != XXH_ERROR);

	auto* layoutHandle = getLayout();
	result = XXH3_64bits_update(gthreadXxhState.get(), &layoutHandle, sizeof(layoutHandle));
	ASSERT(result != XXH_ERROR);

	// todo: hash more state...

	// todo: rendertargets need to use hash key derived from its state and not its handles/pointers, since they are recreated often
	// auto [renderPassHandle, frameBufferHandle] =
	//     static_cast<RenderTarget<Vk>::ValueType>(*getRenderTarget());
	// result = XXH3_64bits_update(threadXXHState.get(), &renderPassHandle, sizeof(renderPassHandle));
	// result = XXH3_64bits_update(threadXXHState.get(), &frameBufferHandle, sizeof(frameBufferHandle));
	// ASSERT(result != XXH_ERROR);

	return XXH3_64bits_digest(gthreadXxhState.get());
}

template <>
void Pipeline<Vk>::InternalPrepareDescriptorSets()
{
	const auto& layout = InternalGetLayout();

	for (const auto& [set, setLayout] : layout.getDescriptorSetLayouts())
	{
		auto insertResultPair = myDescriptorMap.emplace(
			static_cast<DescriptorSetLayoutHandle<Vk>>(setLayout),
			std::make_tuple(
				UpgradableSharedMutex{},
				DescriptorSetStatus::Ready,
				BindingsMap<Vk>{},
				BindingsData<Vk>{},
				DescriptorUpdateTemplate<Vk>{
					getDevice(),
					DescriptorUpdateTemplateCreateDesc<Vk>{
						((setLayout.GetDesc().flags &
						  VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR) != 0u)
							? VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR
							: VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET,
						static_cast<VkDescriptorSetLayout>(setLayout),
						myBindPoint,
						static_cast<VkPipelineLayout>(layout),
						set}},
				((setLayout.GetDesc().flags &
				  VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR) != 0u)
					? std::nullopt
					: std::make_optional(DescriptorSetArrayList<Vk>{})));

		auto& [insertIt, insertResult] = insertResultPair;
		auto& [bindingIndex, bindingTuple] = *insertIt;
		auto& [mutex, setState, bindingsMap, bindingsData, setTemplate, setOptionalArrayList] =
			bindingTuple;

		if (setOptionalArrayList)
		{
			auto& setArrayList = setOptionalArrayList.value();

			setArrayList.emplace_front(std::make_tuple(
				DescriptorSetArray<Vk>(
					getDevice(),
					setLayout,
					DescriptorSetArrayCreateDesc<Vk>{myDescriptorPool}),
				0,
				0));
		}
	}
}

template <>
void Pipeline<Vk>::InternalResetGraphicsState()
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
	myGraphicsState.viewports.emplace_back(Viewport<Vk>{0.0F, 0.0F, 0, 0, 0.0F, 1.0F});

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
		0.0F,
		0.0F,
		0.0F,
		1.0F};

	myGraphicsState.multisample = {
		VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		nullptr,
		0,
		VK_SAMPLE_COUNT_1_BIT,
		VK_FALSE,
		1.0F,
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
		0.0F,
		1.0F};

	myGraphicsState.colorBlendAttachments.clear();
	myGraphicsState.colorBlendAttachments.emplace_back(PipelineColorBlendAttachmentState<Vk>{
		VK_FALSE,
		VK_BLEND_FACTOR_ONE,
		VK_BLEND_FACTOR_ZERO,
		VK_BLEND_OP_ADD,
		VK_BLEND_FACTOR_ONE,
		VK_BLEND_FACTOR_ZERO,
		VK_BLEND_OP_ADD,
		VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
			VK_COLOR_COMPONENT_A_BIT});

	myGraphicsState.colorBlend = {
		VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		nullptr,
		0,
		VK_FALSE,
		VK_LOGIC_OP_COPY,
		static_cast<uint32_t>(myGraphicsState.colorBlendAttachments.size()),
		myGraphicsState.colorBlendAttachments.data(),
		{0.0F, 0.0F, 0.0F, 0.0F}};

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
void Pipeline<Vk>::InternalResetComputeState()
{
	//myComputeState....
}

template <>
void Pipeline<Vk>::InternalResetState()
{
	InternalResetGraphicsState();
	InternalResetComputeState();
}

template <>
PipelineHandle<Vk> Pipeline<Vk>::InternalCreateGraphicsPipeline(uint64_t hashKey)
{
	ZoneScopedN("Pipeline::InternalCreateGraphicsPipeline");

	const auto& layout = InternalGetLayout();
	auto& renderTarget = getRenderTarget();

	VkGraphicsPipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
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
	pipelineInfo.renderPass = std::get<0>(static_cast<RenderTargetHandle<Vk>>(renderTarget));
	pipelineInfo.subpass = 0; // TODO(djohansson): loop through all subpasses?
	pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
	pipelineInfo.basePipelineIndex = -1;

	VkPipeline pipelineHandle;
	VK_CHECK(vkCreateGraphicsPipelines(
		*getDevice(),
		myCache,
		1,
		&pipelineInfo,
		&getDevice()->getInstance()->getHostAllocationCallbacks(),
		&pipelineHandle));

#if (GRAPHICS_VALIDATION_LEVEL > 0)
	{
		getDevice()->AddOwnedObjectHandle(
			getUid(),
			VK_OBJECT_TYPE_PIPELINE,
			reinterpret_cast<uint64_t>(pipelineHandle),
			std::format("{0}_Pipeline_{1}", getName(), hashKey));
	}
#endif

	return pipelineHandle;
}

template <>
PipelineHandle<Vk> Pipeline<Vk>::InternalGetPipeline()
{
	ZoneScopedN("Pipeline::InternalGetPipeline");

	auto [keyValIt, insertResult] =
		myPipelineMap.insert({InternalCalculateHashKey(), nullptr});
	auto& [key, pipelineHandleAtomic] = *keyValIt;

	if (insertResult)
	{
		ZoneScopedN("Pipeline::InternalGetPipeline::store");

		pipelineHandleAtomic.store(
			InternalCreateGraphicsPipeline(key), std::memory_order_release);
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
void Pipeline<Vk>::BindPipeline(
	CommandBufferHandle<Vk> cmd, PipelineBindPoint<Vk> bindPoint, PipelineHandle<Vk> handle) const
{
	ZoneScopedN("Pipeline::BindPipeline");

	vkCmdBindPipeline(cmd, bindPoint, handle);
}

template <>
void Pipeline<Vk>::BindPipelineAuto(CommandBufferHandle<Vk> cmd)
{
	BindPipeline(cmd, myBindPoint, InternalGetPipeline());
}

template <>
void Pipeline<Vk>::SetModel(const std::shared_ptr<Model<Vk>>& model)
{
	myGraphicsState.vertexInput.vertexBindingDescriptionCount =
		static_cast<uint32_t>(model->getBindings().size());
	myGraphicsState.vertexInput.pVertexBindingDescriptions = model->getBindings().data();
	myGraphicsState.vertexInput.vertexAttributeDescriptionCount =
		static_cast<uint32_t>(model->GetDesc().attributes.size());
	myGraphicsState.vertexInput.pVertexAttributeDescriptions = model->GetDesc().attributes.data();

	SetDescriptorData(
		"g_vertexBuffer",
		DescriptorBufferInfo<Vk>{model->getVertexBuffer(), 0, VK_WHOLE_SIZE},
		DescriptorSetCategory_GlobalBuffers);

	myGraphicsState.resources.model = model;
}

template <>
PipelineLayoutHandle<Vk> Pipeline<Vk>::CreateLayout(const ShaderSet<Vk>& shaderSet)
{
	auto layout = PipelineLayout<Vk>(getDevice(), shaderSet);
	auto* handle = static_cast<PipelineLayoutHandle<Vk>>(layout);

	myGraphicsState.layouts.emplace(handle, std::move(layout));
	
	return handle;
}

template <>
void Pipeline<Vk>::BindLayoutAuto(PipelineLayoutHandle<Vk> layout, PipelineBindPoint<Vk> bindPoint)
{
	myLayout = layout;
	myBindPoint = bindPoint;

	const auto& shaderModules = InternalGetLayout().GetShaderModules();

	ASSERT(!shaderModules.empty());

	switch (myBindPoint)
	{
	case VK_PIPELINE_BIND_POINT_GRAPHICS: {
		myGraphicsState.shaderStageFlags = {};
		myGraphicsState.shaderStages.reserve(shaderModules.size());

		for (const auto& shader : shaderModules)
		{
			const auto& [entryPointName, shaderStage] = shader.GetEntryPoint();

			if ((shaderStage & VK_SHADER_STAGE_ALL_GRAPHICS) != 0)
			{
				myGraphicsState.shaderStages.emplace_back(PipelineShaderStageCreateInfo<Vk>{
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
		ASSERTF(false, "Not implemented");
		break;
	};

	InternalPrepareDescriptorSets();
}

template <>
void Pipeline<Vk>::SetRenderTarget(const std::shared_ptr<RenderTarget<Vk>>& renderTarget)
{
	auto extent = renderTarget ? renderTarget->GetRenderTargetDesc().extent : Extent2d<Vk>{0, 0};

	myGraphicsState.viewports[0].width = static_cast<float>(extent.width);
	myGraphicsState.viewports[0].height = static_cast<float>(extent.height);
	myGraphicsState.scissorRects[0].offset = {0, 0};
	myGraphicsState.scissorRects[0].extent = {extent.width, extent.height};

	myRenderTarget = renderTarget;
}

template <>
void Pipeline<Vk>::InternalUpdateDescriptorSet(
	const DescriptorSetLayout<Vk>& setLayout,
	const BindingsData<Vk>& bindingsData,
	const DescriptorUpdateTemplate<Vk>& setTemplate,
	DescriptorSetArrayList<Vk>& setArrayList)
{
	ZoneScopedN("Pipeline::InternalUpdateDescriptorSet");

	bool setArrayListIsEmpty = setArrayList.empty();
	bool frontArrayIsFull = setArrayListIsEmpty
								? false
								: std::get<1>(setArrayList.front()) ==
									  (std::get<0>(setArrayList.front()).Capacity() - 1);

	if (setArrayListIsEmpty || frontArrayIsFull)
	{
		setArrayList.emplace_front(std::make_tuple(
			DescriptorSetArray<Vk>(
				getDevice(), setLayout, DescriptorSetArrayCreateDesc<Vk>{myDescriptorPool}),
			~0,
			0));
	}

	auto& [setArray, setIndex, setRefCount] = setArrayList.front();
	++setIndex;
	ASSERT(setIndex < setArray.Capacity());
	auto* setHandle = setArray[setIndex];

	{
		ZoneScopedN(
			"Pipeline::InternalUpdateDescriptorSet::vkUpdateDescriptorSetWithTemplate");

		vkUpdateDescriptorSetWithTemplate(
			*getDevice(), setHandle, setTemplate, bindingsData.data());
	}

	// clean up
	if (auto setArrayIt = setArrayList.begin(); setArrayIt != setArrayList.end())
	{
		setArrayIt++;
		while (setArrayIt != setArrayList.end())
		{
			auto& [setArray, setIndex, setRefCount] = *setArrayIt;
			if (setRefCount == 0u)
				setArrayIt = setArrayList.erase(setArrayIt);
			else
				setArrayIt++;
		}
	}
}

template <>
void Pipeline<Vk>::InternalPushDescriptorSet(
	CommandBufferHandle<Vk> cmd,
	const BindingsData<Vk>& bindingsData,
	const DescriptorUpdateTemplate<Vk>& setTemplate) const
{
	ZoneScopedN("Pipeline::InternalPushDescriptorSet");

	{
		ZoneScopedN(
			"Pipeline::InternalPushDescriptorSet::vkCmdPushDescriptorSetWithTemplateKHR");

		if (pipeline::gVkCmdPushDescriptorSetWithTemplateKHR == nullptr)
			pipeline::gVkCmdPushDescriptorSetWithTemplateKHR =
				reinterpret_cast<PFN_vkCmdPushDescriptorSetWithTemplateKHR>(
					vkGetDeviceProcAddr(*getDevice(), "vkCmdPushDescriptorSetWithTemplateKHR"));

		pipeline::gVkCmdPushDescriptorSetWithTemplateKHR(
			cmd, setTemplate, getLayout(), setTemplate.GetDesc().set, bindingsData.data());
	}
}

template <>
void Pipeline<Vk>::InternalUpdateDescriptorSetTemplate(
	const BindingsMap<Vk>& bindingsMap, DescriptorUpdateTemplate<Vk>& setTemplate)
{
	ZoneScopedN("Pipeline::InternalUpdateDescriptorSetTemplate");

	std::vector<DescriptorUpdateTemplateEntry<Vk>> entries;
	entries.reserve(bindingsMap.size());

	for (const auto& [index, binding] : bindingsMap)
	{
		const auto& [offset, count, type, ranges] = binding;

		uint32_t rangeOffset = 0UL;

		for (const auto& [low, high] : ranges)
		{
			auto rangeCount = high - low;

			entries.emplace_back(DescriptorUpdateTemplateEntry<Vk>{
				index,
				low,
				rangeCount,
				type,
				(offset + rangeOffset) * sizeof(BindingVariant<Vk>),
				sizeof(BindingVariant<Vk>)});

			rangeOffset += rangeCount;
		}
	}

	setTemplate.SetEntries(std::move(entries));
}

template <>
void Pipeline<Vk>::BindDescriptorSet(
	CommandBufferHandle<Vk> cmd,
	DescriptorSetHandle<Vk> handle,
	PipelineBindPoint<Vk> bindPoint,
	PipelineLayoutHandle<Vk> layoutHandle,
	uint32_t set,
	std::optional<uint32_t> bufferOffset) const
{
	ZoneScopedN("Pipeline::BindDescriptorSet");

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
void Pipeline<Vk>::BindDescriptorSetAuto(
	CommandBufferHandle<Vk> cmd, uint32_t set, std::optional<uint32_t> bufferOffset)
{
	ZoneScopedN("Pipeline::BindDescriptorSetAuto");

	const auto& layout = InternalGetLayout();
	const auto& setLayout = layout.getDescriptorSetLayout(set);
	auto& [mutex, setState, bindingsMap, bindingsData, setTemplate, setOptionalArrayList] =
		myDescriptorMap.at(setLayout);

	mutex.lock_upgrade();

	if (setState == DescriptorSetStatus::Dirty && setOptionalArrayList)
	{
		mutex.unlock_upgrade_and_lock();

		InternalUpdateDescriptorSet(
			setLayout, bindingsData, setTemplate, setOptionalArrayList.value());

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
		ASSERT(!setArrayList.empty());
		auto& [setArray, setIndex, setRefCount] = setArrayList.front();
		auto* handle = setArray[setIndex];

		BindDescriptorSet(
			cmd,
			handle,
			myBindPoint,
			static_cast<PipelineLayoutHandle<Vk>>(layout),
			set,
			bufferOffset);

		setRefCount++;

		// getDevice()->AddTimelineCallback([refCountPtr = &setRefCount](uint64_t)
		// 										{ (*refCountPtr)--; });
	}
	else
	{
		InternalPushDescriptorSet(cmd, bindingsData, setTemplate);
	}

	mutex.unlock_shared();
}

template <>
Pipeline<Vk>::Pipeline(
	const std::shared_ptr<Device<Vk>>& device, PipelineConfiguration<Vk>&& defaultConfig)
	: DeviceObject(device, {})
	, myConfig{std::get<std::filesystem::path>(Application::Instance().lock()->Env().variables["UserProfilePath"]) / "pipeline.json", std::forward<PipelineConfiguration<Vk>>(defaultConfig)}
	, myDescriptorPool(
		  [](const std::shared_ptr<Device<Vk>>& device)
		  {
			  const uint32_t descBaseCount = 1024;
			  VkDescriptorPoolSize poolSizes[]{
				  {VK_DESCRIPTOR_TYPE_SAMPLER, descBaseCount * ShaderTypes_GlobalSamplerCount},
				  {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				   descBaseCount * ShaderTypes_GlobalSamplerCount},
				  {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
				   descBaseCount * ShaderTypes_GlobalTextureCount},
				  {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
				   descBaseCount * ShaderTypes_GlobalRWTextureCount},
				  {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, descBaseCount},
				  {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, descBaseCount},
				  {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, descBaseCount},
				  {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, descBaseCount},
				  {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, descBaseCount},
				  {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, descBaseCount},
				  {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, descBaseCount}};

			  VkDescriptorPoolInlineUniformBlockCreateInfo inlineUniformBlockInfo{
				  VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_INLINE_UNIFORM_BLOCK_CREATE_INFO};
			  inlineUniformBlockInfo.maxInlineUniformBlockBindings = descBaseCount;

			  VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
			  poolInfo.pNext = &inlineUniformBlockInfo;
			  poolInfo.poolSizeCount = std::size(poolSizes);
			  poolInfo.pPoolSizes = poolSizes;
			  poolInfo.maxSets = descBaseCount * std::size(poolSizes);
			  poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
			  // VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT
			  // VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT

			  VkDescriptorPool outDescriptorPool;
			  VK_CHECK(vkCreateDescriptorPool(
				  *device,
				  &poolInfo,
				  &device->getInstance()->getHostAllocationCallbacks(),
				  &outDescriptorPool));

			  return outDescriptorPool;
		  }(device))
	, myCache(pipeline::LoadPipelineCache(myConfig.cachePath, device))
{
	// todo: refactor, since this will be called to excessivly
	InternalResetState();

#if (GRAPHICS_VALIDATION_LEVEL > 0)
	device->AddOwnedObjectHandle(
		getUid(),
		VK_OBJECT_TYPE_PIPELINE_CACHE,
		reinterpret_cast<uint64_t>(myCache),
		std::format("{0}_PipelineCache", getName()));

	device->AddOwnedObjectHandle(
		getUid(),
		VK_OBJECT_TYPE_DESCRIPTOR_POOL,
		reinterpret_cast<uint64_t>(myDescriptorPool),
		"Device_DescriptorPool");
#endif
}

template <>
Pipeline<Vk>::~Pipeline()
{
	if (auto fileInfo = pipeline::SavePipelineCache(
			myConfig.cachePath,
			*getDevice(),
			getDevice()->getPhysicalDeviceInfo().deviceProperties,
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
		vkDestroyPipeline(
			*getDevice(),
			pipelineIt.second,
			&getDevice()->getInstance()->getHostAllocationCallbacks());

	vkDestroyPipelineCache(
		*getDevice(),
		myCache,
		&getDevice()->getInstance()->getHostAllocationCallbacks());

	myGraphicsState.resources = {};
	myDescriptorMap.clear();

	if (myDescriptorPool != nullptr)
		vkDestroyDescriptorPool(*getDevice(), myDescriptorPool, &getDevice()->getInstance()->getHostAllocationCallbacks());
}
