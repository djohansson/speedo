#include "../pipeline.h"
#include "../shader.h"
#include "../rhiapplication.h"

#include "../shaders/capi.h"

#include "utils.h"

#include <format>
#include <print>

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

using namespace file;

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

PipelineCacheHandle<kVk> LoadPipelineCache(
	const std::filesystem::path& cacheFilePath, const std::shared_ptr<Device<kVk>>& device)
{
	std::vector<char> cacheData;

	auto loadCacheOp = [&device, &cacheData](auto& inStream) -> std::error_code
	{
		if (auto result = inStream(cacheData); failure(result))
			return std::make_error_code(result);

		const auto* header = reinterpret_cast<const PipelineCacheHeader<kVk>*>(cacheData.data());

		if (cacheData.empty() ||
			!IsCacheValid(*header, device->GetPhysicalDeviceInfo().deviceProperties))
		{
			std::cerr << "Invalid pipeline cache, creating new." << '\n';
			cacheData.clear();
		}

		return {};
	};

	if (auto fileInfo = GetRecord<false>(cacheFilePath); fileInfo)
		fileInfo = LoadBinary<false>(cacheFilePath, loadCacheOp);

	VkPipelineCacheCreateInfo createInfo{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
	createInfo.initialDataSize = cacheData.size();
	createInfo.pInitialData =
		(static_cast<unsigned int>(!cacheData.empty()) != 0U) ? cacheData.data() : nullptr;

	PipelineCacheHandle<kVk> cache;
	VK_CHECK(vkCreatePipelineCache(
		*device,
		&createInfo,
		&device->GetInstance()->GetHostAllocationCallbacks(),
		&cache));

	return cache;
}

std::vector<char>
GetPipelineCacheData(DeviceHandle<kVk> device, PipelineCacheHandle<kVk> pipelineCache)
{
	std::vector<char> cacheData;
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

static PFN_vkCmdPushDescriptorSetWithTemplateKHR gVkCmdPushDescriptorSetWithTemplateKHR{};

} // namespace pipeline

template <>
PipelineLayout<kVk>& PipelineLayout<kVk>::operator=(PipelineLayout<kVk>&& other) noexcept
{
	DeviceObject::operator=(std::forward<PipelineLayout<kVk>>(other));
	myShaderModules = std::exchange(other.myShaderModules, {});
	myDescriptorSetLayouts = std::exchange(other.myDescriptorSetLayouts, {});
	myLayout = std::exchange(other.myLayout, {});
	return *this;
}

template <>
PipelineLayout<kVk>::PipelineLayout(PipelineLayout<kVk>&& other) noexcept
	: DeviceObject(std::forward<PipelineLayout<kVk>>(other))
	, myShaderModules(std::exchange(other.myShaderModules, {}))
	, myDescriptorSetLayouts(std::exchange(other.myDescriptorSetLayouts, {}))
	, myLayout(std::exchange(other.myLayout, {}))
{}

template <>
PipelineLayout<kVk>::PipelineLayout(
	const std::shared_ptr<Device<kVk>>& device,
	std::vector<ShaderModule<kVk>>&& shaderModules,
	DescriptorSetLayoutFlatMap<kVk>&& descriptorSetLayouts,
	PipelineLayoutHandle<kVk>&& layout)
	: DeviceObject(
		  device,
		  {"_PipelineLayout"},
		  1,
		  VK_OBJECT_TYPE_PIPELINE_LAYOUT,
		  reinterpret_cast<uint64_t*>(&layout))
	, myShaderModules(std::exchange(shaderModules, {}))
	, myDescriptorSetLayouts(std::exchange(descriptorSetLayouts, {}))
	, myLayout(std::forward<PipelineLayoutHandle<kVk>>(layout))
{}

template <>
PipelineLayout<kVk>::PipelineLayout(
	const std::shared_ptr<Device<kVk>>& device,
	std::vector<ShaderModule<kVk>>&& shaderModules,
	DescriptorSetLayoutFlatMap<kVk>&& descriptorSetLayouts)
	: PipelineLayout(
		  device,
		  std::forward<std::vector<ShaderModule<kVk>>>(shaderModules),
		  std::forward<DescriptorSetLayoutFlatMap<kVk>>(descriptorSetLayouts),
		  [&descriptorSetLayouts, &device]
		  {
			  // todo: rewrite flatmap so that keys and vals are stored as separate arrays so that we dont have to make this conversion
			  auto handles = descriptorset::GetDescriptorSetLayoutHandles<kVk>(descriptorSetLayouts);
			  auto pushConstantRanges =
				  descriptorset::GetPushConstantRanges<kVk>(descriptorSetLayouts);

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
				  &device->GetInstance()->GetHostAllocationCallbacks(),
				  &layout));

			  return layout;
		  }())
{}

template <>
PipelineLayout<kVk>::PipelineLayout(
	const std::shared_ptr<Device<kVk>>& device, const ShaderSet<kVk>& shaderSet)
	: PipelineLayout(
		  device,
		  [&shaderSet, &device]
		  {
			  std::vector<ShaderModule<kVk>> shaderModules;
			  shaderModules.reserve(shaderSet.shaders.size());
			  for (const auto& shader : shaderSet.shaders)
				  shaderModules.emplace_back(device, shader);
			  return shaderModules;
		  }(),
		  [&shaderSet, &device]
		  {
			  DescriptorSetLayoutFlatMap<kVk> map;
			  for (auto [set, layout] : shaderSet.layouts)
				  map.emplace(set, DescriptorSetLayout<kVk>(device, std::move(layout)));
			  return map;
		  }())
{}

template <>
PipelineLayout<kVk>::~PipelineLayout()
{
	if (myLayout != nullptr)
		vkDestroyPipelineLayout(
			*InternalGetDevice(),
			myLayout,
			&InternalGetDevice()->GetInstance()->GetHostAllocationCallbacks());
}

template <>
void PipelineLayout<kVk>::Swap(PipelineLayout& rhs) noexcept
{
	DeviceObject::Swap(rhs);
	std::swap(myShaderModules, rhs.myShaderModules);
	std::swap(myDescriptorSetLayouts, rhs.myDescriptorSetLayouts);
	std::swap(myLayout, rhs.myLayout);
}

template <>
uint64_t Pipeline<kVk>::InternalCalculateHashKey() const
{
	ZoneScopedN("Pipeline::InternalCalculateHashKey");

	thread_local std::unique_ptr<XXH3_state_t, XXH_errorcode (*)(XXH3_state_t*)> gThreadXxhState{
		XXH3_createState(), XXH3_freeState};

	auto result = XXH3_64bits_reset(gThreadXxhState.get());
	ASSERT(result != XXH_ERROR);

	result = XXH3_64bits_update(gThreadXxhState.get(), &myBindPoint, sizeof(myBindPoint));
	ASSERT(result != XXH_ERROR);

	auto layoutIt = InternalGetLayout();
	CHECK(layoutIt != myLayouts.end());
	auto layoutHandle = static_cast<PipelineLayoutHandle<kVk>>(*layoutIt);
	result = XXH3_64bits_update(gThreadXxhState.get(), &layoutHandle, sizeof(layoutHandle));
	//result = XXH3_64bits_update(gThreadXxhState.get(), &(*layoutIt), sizeof(*layoutIt));
	ASSERT(result != XXH_ERROR);

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
	CHECK(layoutIt != myLayouts.end());
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
					UpgradableSharedMutex{},
					DescriptorSetStatus::kReady,
					BindingsMap<kVk>{},
					BindingsData<kVk>{},
					DescriptorUpdateTemplate<kVk>{
						InternalGetDevice(),
						DescriptorUpdateTemplateCreateDesc<kVk>{
							((setLayout.GetDesc().flags &
							VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR) != 0U)
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
			ASSERT(insertResult);
			ASSERT(insertIt != myDescriptorMap.end());
			auto& [bindingIndex, bindingTuple] = *insertIt;
			auto& [mutex, setState, bindingsMap, bindingsData, setTemplate, setOptionalArrayList] = bindingTuple;

			if (setOptionalArrayList)
			{
				auto& setArrayList = setOptionalArrayList.value();

				setArrayList.emplace_front(std::make_tuple(
					DescriptorSetArray<kVk>(
						InternalGetDevice(),
						setLayout,
						DescriptorSetArrayCreateDesc<kVk>{myDescriptorPool}),
					0));
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
	myGraphicsState.viewports.emplace_back(Viewport<kVk>{0.0F, 0.0F, 0, 0, 0.0F, 1.0F});

	myGraphicsState.scissorRects.clear();
	myGraphicsState.scissorRects.emplace_back(Rect2D<kVk>{{0, 0}, {0, 0}});

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
	myGraphicsState.colorBlendAttachments.emplace_back(PipelineColorBlendAttachmentState<kVk>{
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
void Pipeline<kVk>::InternalResetComputeState()
{
	//myComputeState....
}

template <>
void Pipeline<kVk>::InternalResetDescriptorPool()
{
	vkResetDescriptorPool(*InternalGetDevice(), myDescriptorPool, 0);
}

template <>
PipelineHandle<kVk> Pipeline<kVk>::InternalCreateGraphicsPipeline(uint64_t hashKey)
{
	ZoneScopedN("Pipeline::InternalCreateGraphicsPipeline");

	const auto layoutIt = InternalGetLayout();
	CHECK(layoutIt != myLayouts.end());
	const auto& layout = *layoutIt;
	auto& renderTarget = GetRenderTarget();

	VkGraphicsPipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, nullptr, 0};
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
	pipelineInfo.renderPass = std::get<0>(static_cast<RenderTargetHandle<kVk>>(renderTarget));
	pipelineInfo.subpass = 0; // TODO(djohansson): loop through all subpasses?
	pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
	pipelineInfo.basePipelineIndex = -1;

	VkPipeline pipelineHandle;
	VK_CHECK(vkCreateGraphicsPipelines(
		*InternalGetDevice(),
		myCache,
		1,
		&pipelineInfo,
		&InternalGetDevice()->GetInstance()->GetHostAllocationCallbacks(),
		&pipelineHandle));

#if (GRAPHICS_VALIDATION_LEVEL > 0)
	{
		InternalGetDevice()->AddOwnedObjectHandle(
			GetUid(),
			VK_OBJECT_TYPE_PIPELINE,
			reinterpret_cast<uint64_t>(pipelineHandle),
			std::format("{}_Pipeline_{}", GetName(), hashKey));
	}
#endif

	return pipelineHandle;
}

template <>
PipelineHandle<kVk> Pipeline<kVk>::InternalCreateComputePipeline(uint64_t hashKey)
{
	ZoneScopedN("Pipeline::InternalCreateComputePipeline");

	const auto layoutIt = InternalGetLayout();
	CHECK(layoutIt != myLayouts.end());
	const auto& layout = *layoutIt;

	VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, nullptr, 0};
	pipelineInfo.stage = myComputeState.shaderStage;
	pipelineInfo.layout = layout;
	pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
	pipelineInfo.basePipelineIndex = -1;

	VkPipeline pipelineHandle;
	VK_CHECK(vkCreateComputePipelines(
		*InternalGetDevice(),
		myCache,
		1,
		&pipelineInfo,
		&InternalGetDevice()->GetInstance()->GetHostAllocationCallbacks(),
		&pipelineHandle));
  
	return pipelineHandle;
}

template <>
PipelineHandle<kVk> Pipeline<kVk>::InternalGetPipeline()
{
	ZoneScopedN("Pipeline::InternalGetPipeline");

	auto [keyValIt, insertResult] =
		myPipelineMap.insert({InternalCalculateHashKey(), CopyableAtomic<PipelineHandle<kVk>>{}});
	auto& [key, pipelineHandleAtomic] = *keyValIt;

	if (insertResult)
	{
		ZoneScopedN("Pipeline::InternalGetPipeline::store");

		switch (myBindPoint)
		{
		case VK_PIPELINE_BIND_POINT_GRAPHICS:
			pipelineHandleAtomic.store(InternalCreateGraphicsPipeline(key), std::memory_order_release);
			break;
		case VK_PIPELINE_BIND_POINT_COMPUTE:
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
	CommandBufferHandle<kVk> cmd, PipelineBindPoint<kVk> bindPoint, PipelineHandle<kVk> handle) const
{
	ZoneScopedN("Pipeline::BindPipeline");

	vkCmdBindPipeline(cmd, bindPoint, handle);
}

template <>
PipelineHandle<kVk> Pipeline<kVk>::BindPipelineAuto(CommandBufferHandle<kVk> cmd)
{
	auto handle = InternalGetPipeline();

	BindPipeline(cmd, myBindPoint, handle);
	
	return handle;
}

template <>
std::shared_ptr<Model<kVk>> Pipeline<kVk>::SetModel(const std::shared_ptr<Model<kVk>>& model)
{
	myGraphicsState.vertexInput.vertexBindingDescriptionCount =
		static_cast<uint32_t>(model->GetBindings().size());
	myGraphicsState.vertexInput.pVertexBindingDescriptions = model->GetBindings().data();
	myGraphicsState.vertexInput.vertexAttributeDescriptionCount =
		static_cast<uint32_t>(model->GetDesc().attributes.size());
	myGraphicsState.vertexInput.pVertexAttributeDescriptions = model->GetDesc().attributes.data();

	SetDescriptorData(
		"gVertexBuffer",
		DescriptorBufferInfo<kVk>{model->GetVertexBuffer(), 0, VK_WHOLE_SIZE},
		DESCRIPTOR_SET_CATEGORY_GLOBAL_BUFFERS);

	auto oldModel = myResources.model;
	
	myResources.model = model;

	return oldModel;
}

template <>
PipelineLayoutHandle<kVk> Pipeline<kVk>::GetLayout() const noexcept
{
	const auto layoutIt = InternalGetLayout();
	
	if (layoutIt == myLayouts.end())
		return VK_NULL_HANDLE;

	return static_cast<PipelineLayoutHandle<kVk>>(*layoutIt);
}

template <>
PipelineLayoutHandle<kVk> Pipeline<kVk>::CreateLayout(const ShaderSet<kVk>& shaderSet)
{
	const auto& [layoutIt, wasInserted] = myLayouts.emplace(PipelineLayout<kVk>(InternalGetDevice(), shaderSet));

	return static_cast<PipelineLayoutHandle<kVk>>(*layoutIt);
}

template <>
void Pipeline<kVk>::BindLayoutAuto(PipelineLayoutHandle<kVk> layoutHandle, PipelineBindPoint<kVk> bindPoint)
{
	myBindPoint = bindPoint;
	myCurrentLayoutIt = myLayouts.find(layoutHandle);
	CHECK(myCurrentLayoutIt != myLayouts.end());
	const auto& layout = *myCurrentLayoutIt;
	const auto& shaderModules = layout.GetShaderModules();

	ASSERT(!shaderModules.empty());

	switch (myBindPoint)
	{
	case VK_PIPELINE_BIND_POINT_GRAPHICS:
		myGraphicsState.shaderStageFlags = {};
		myGraphicsState.shaderStages.clear();
		myGraphicsState.shaderStages.reserve(shaderModules.size());
		for (const auto& shader : shaderModules)
		{
			const auto& [entryPointName, shaderStage, launchParams] = shader.GetEntryPoint();

			if ((shaderStage & VK_SHADER_STAGE_ALL_GRAPHICS) != 0)
			{
				myGraphicsState.shaderStages.emplace_back(PipelineShaderStageCreateInfo<kVk>{
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
		break;
	case VK_PIPELINE_BIND_POINT_COMPUTE:
		{
			const auto& [entryPointName, shaderStage, launchParams] = shaderModules.front().GetEntryPoint();
			CHECK(shaderStage == VK_SHADER_STAGE_COMPUTE_BIT);
			myComputeState.shaderStage = {
				VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
				nullptr,
				0,
				VK_SHADER_STAGE_COMPUTE_BIT,
				shaderModules.front(),
				entryPointName.c_str(),
				nullptr};
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
void Pipeline<kVk>::SetRenderTarget(const std::shared_ptr<RenderTarget<kVk>>& renderTarget)
{
	auto extent = renderTarget ? renderTarget->GetRenderTargetDesc().extent : Extent2d<kVk>{0, 0};

	myGraphicsState.viewports[0].width = static_cast<float>(extent.width);
	myGraphicsState.viewports[0].height = static_cast<float>(extent.height);
	myGraphicsState.scissorRects[0].offset = {0, 0};
	myGraphicsState.scissorRects[0].extent = {extent.width, extent.height};

	myRenderTarget = renderTarget;
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
				InternalGetDevice(), setLayout, DescriptorSetArrayCreateDesc<kVk>{myDescriptorPool}),
			~0));
	}

	auto& [setArray, setIndex] = setArrayList.front();
	++setIndex;
	ASSERT(setIndex < setArray.Capacity());
	auto* setHandle = setArray[setIndex];

	{
		ZoneScopedN(
			"Pipeline::InternalUpdateDescriptorSet::vkUpdateDescriptorSetWithTemplate");

		vkUpdateDescriptorSetWithTemplate(
			*InternalGetDevice(), setHandle, setTemplate, bindingsData.data());
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

	pipeline::gVkCmdPushDescriptorSetWithTemplateKHR(
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
				index,
				low,
				rangeCount,
				type,
				(offset + rangeOffset) * sizeof(BindingVariant<kVk>),
				sizeof(BindingVariant<kVk>)});

			rangeOffset += rangeCount;
		}
	}

	setTemplate.SetEntries(std::move(entries));
}

template <>
void Pipeline<kVk>::BindDescriptorSet(
	CommandBufferHandle<kVk> cmd,
	DescriptorSetHandle<kVk> handle,
	PipelineBindPoint<kVk> bindPoint,
	PipelineLayoutHandle<kVk> layoutHandle,
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
void Pipeline<kVk>::BindDescriptorSetAuto(
	CommandBufferHandle<kVk> cmd,
	uint32_t set,
	std::optional<uint32_t> bufferOffset)
{
	ZoneScopedN("Pipeline::BindDescriptorSetAuto");

	const auto layoutIt = InternalGetLayout();
	CHECK(layoutIt != myLayouts.end());
	const auto& layout = *layoutIt;
	const auto& setLayout = layout.GetDescriptorSetLayout(set);
	auto& [mutex, setState, bindingsMap, bindingsData, setTemplate, setOptionalArrayList] =
		myDescriptorMap.at(setLayout);

	mutex.lock_upgrade();

	if (setState == DescriptorSetStatus::kDirty && setOptionalArrayList)
	{
		mutex.unlock_upgrade_and_lock();

		InternalUpdateDescriptorSet(
			setLayout, bindingsData, setTemplate, setOptionalArrayList.value());

		setState = DescriptorSetStatus::kReady;

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
	else
	{
		InternalPushDescriptorSet(
			cmd,
			static_cast<PipelineLayoutHandle<kVk>>(layout),
			bindingsData,
			setTemplate);
	}

	mutex.unlock_shared();
}

template <>
Pipeline<kVk>::Pipeline(
	const std::shared_ptr<Device<kVk>>& device, PipelineConfiguration<kVk>&& defaultConfig)
	: DeviceObject(device, {})
	, myConfig{std::get<std::filesystem::path>(Application::Instance().lock()->Env().variables["UserProfilePath"]) / "pipeline.json", std::forward<PipelineConfiguration<kVk>>(defaultConfig)}
	, myDescriptorPool(
		  [](const std::shared_ptr<Device<kVk>>& device)
		  {
			  static constexpr uint32_t kDescBaseCount = 1024;
			  static constexpr auto kPoolSizes = std::to_array<VkDescriptorPoolSize>({
				  {VK_DESCRIPTOR_TYPE_SAMPLER, kDescBaseCount * DESCRIPTOR_SET_CATEGORY_GLOBAL_SAMPLERS},
				  {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				   kDescBaseCount * DESCRIPTOR_SET_CATEGORY_GLOBAL_SAMPLERS},
				  {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
				   kDescBaseCount * SHADER_TYPES_GLOBAL_TEXTURE_COUNT},
				  {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
				   kDescBaseCount * SHADER_TYPES_GLOBAL_RW_TEXTURE_COUNT},
				  {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, kDescBaseCount},
				  {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, kDescBaseCount},
				  {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kDescBaseCount},
				  {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kDescBaseCount},
				  {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, kDescBaseCount},
				  {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, kDescBaseCount},
				  {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, kDescBaseCount}});

			  VkDescriptorPoolInlineUniformBlockCreateInfo inlineUniformBlockInfo{
				  VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_INLINE_UNIFORM_BLOCK_CREATE_INFO};
			  inlineUniformBlockInfo.maxInlineUniformBlockBindings = kDescBaseCount;

			  VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
			  poolInfo.pNext = &inlineUniformBlockInfo;
			  poolInfo.poolSizeCount = std::size(kPoolSizes);
			  poolInfo.pPoolSizes = kPoolSizes.data();
			  poolInfo.maxSets = kDescBaseCount * std::size(kPoolSizes);
			  poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
			  // VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT
			  // VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT

			  VkDescriptorPool outDescriptorPool;
			  VK_CHECK(vkCreateDescriptorPool(
				  *device,
				  &poolInfo,
				  &device->GetInstance()->GetHostAllocationCallbacks(),
				  &outDescriptorPool));

			  return outDescriptorPool;
		  }(device))
	, myCache(pipeline::LoadPipelineCache(myConfig.cachePath, device))
{
	if (pipeline::gVkCmdPushDescriptorSetWithTemplateKHR == nullptr)
	pipeline::gVkCmdPushDescriptorSetWithTemplateKHR =
		reinterpret_cast<PFN_vkCmdPushDescriptorSetWithTemplateKHR>(
			vkGetDeviceProcAddr(*InternalGetDevice(), "vkCmdPushDescriptorSetWithTemplateKHR"));

	InternalResetGraphicsState();
	InternalResetComputeState();

#if (GRAPHICS_VALIDATION_LEVEL > 0)
	device->AddOwnedObjectHandle(
		GetUid(),
		VK_OBJECT_TYPE_PIPELINE_CACHE,
		reinterpret_cast<uint64_t>(myCache),
		std::format("{}_PipelineCache", GetName()));

	device->AddOwnedObjectHandle(
		GetUid(),
		VK_OBJECT_TYPE_DESCRIPTOR_POOL,
		reinterpret_cast<uint64_t>(myDescriptorPool),
		"Device_DescriptorPool");
#endif
}

template <>
Pipeline<kVk>::~Pipeline()
{
	if (auto fileInfo = pipeline::SavePipelineCache(
			myConfig.cachePath,
			*InternalGetDevice(),
			InternalGetDevice()->GetPhysicalDeviceInfo().deviceProperties,
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
			*InternalGetDevice(),
			pipelineIt.second,
			&InternalGetDevice()->GetInstance()->GetHostAllocationCallbacks());

	vkDestroyPipelineCache(
		*InternalGetDevice(),
		myCache,
		&InternalGetDevice()->GetInstance()->GetHostAllocationCallbacks());

	myResources = {};
	myDescriptorMap.clear();

	if (myDescriptorPool != nullptr)
		vkDestroyDescriptorPool(*InternalGetDevice(), myDescriptorPool, &InternalGetDevice()->GetInstance()->GetHostAllocationCallbacks());
}
