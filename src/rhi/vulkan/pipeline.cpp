#include <rhi/crashreport.h>
#include <rhi/pipeline.h>
#include <rhi/rhi.h>
#include <rhi/vulkan/convert.h>
#include <rhi/vulkan/pipelinecache.h>
#include <rhi/vulkan/utils.h>

#include <core/file.h>

#include <filesystem>
#include <format>
#include <print>

namespace rhi
{

IMPLEMENT_OBJECT_GETINSTANCE(Pipeline<kVk>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(Pipeline<kVk>);

template <>
uint64_t Pipeline<kVk>::InternalCalculateHashKey(GraphicsPipelineVariant variant, ComputePipelineVariant computeVariant) const
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
		result = XXH3_64bits_update(gThreadXxhState.get(), &variant, sizeof(variant));
		ENSURE(result != XXH_ERROR);

		// and the render target's attachment formats: the same variant needs a pipeline per render target layout (e.g.
		// the main pass's, and the depth only shadow atlas's)
		if (const auto& rendering = myGraphicsState.dynamicRendering)
		{
			result = XXH3_64bits_update(gThreadXxhState.get(), &rendering->colorAttachmentCount, sizeof(rendering->colorAttachmentCount));
			ENSURE(result != XXH_ERROR);
			result = XXH3_64bits_update(
				gThreadXxhState.get(), rendering->pColorAttachmentFormats, rendering->colorAttachmentCount * sizeof(VkFormat));
			ENSURE(result != XXH_ERROR);
			result = XXH3_64bits_update(gThreadXxhState.get(), &rendering->depthAttachmentFormat, sizeof(rendering->depthAttachmentFormat));
			ENSURE(result != XXH_ERROR);
			result = XXH3_64bits_update(gThreadXxhState.get(), &rendering->stencilAttachmentFormat, sizeof(rendering->stencilAttachmentFormat));
			ENSURE(result != XXH_ERROR);
		}
	}

	if (myBindPoint == PipelineBindPoint::kCompute)
	{
		result = XXH3_64bits_update(gThreadXxhState.get(), &computeVariant, sizeof(computeVariant));
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
		.pVertexAttributeDescriptions = nullptr,};

	myGraphicsState.inputAssembly = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.pNext = nullptr,
		.flags = 0,
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
		.primitiveRestartEnable = VK_FALSE,};

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
		.pScissors = myGraphicsState.scissorRects.data(),};

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
		.lineWidth = 1.0F,};

	myGraphicsState.multisample = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.pNext = nullptr,
		.flags = 0,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
		.sampleShadingEnable = VK_FALSE,
		.minSampleShading = 1.0F,
		.pSampleMask = nullptr,
		.alphaToCoverageEnable = VK_FALSE,
		.alphaToOneEnable = VK_FALSE,};

	myGraphicsState.depthStencil = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		.pNext = nullptr,
		.flags = 0,
		.depthTestEnable = VK_TRUE,
		.depthWriteEnable = VK_TRUE,
		.depthCompareOp = VK_COMPARE_OP_LESS,
		.depthBoundsTestEnable = VK_FALSE,
		// what writes depth writes the stencil's first bit too, the reference (dynamic: CommandEncoder::SetStencilReference)
		.stencilTestEnable = VK_FALSE,
		.front = {.failOp = VK_STENCIL_OP_KEEP, .passOp = VK_STENCIL_OP_REPLACE, .depthFailOp = VK_STENCIL_OP_KEEP, .compareOp = VK_COMPARE_OP_ALWAYS, .compareMask = 0xFFU, .writeMask = 1U, .reference = 0U},
		.back = {.failOp = VK_STENCIL_OP_KEEP, .passOp = VK_STENCIL_OP_REPLACE, .depthFailOp = VK_STENCIL_OP_KEEP, .compareOp = VK_COMPARE_OP_ALWAYS, .compareMask = 0xFFU, .writeMask = 1U, .reference = 0U},
		.minDepthBounds = 0.0F,
		.maxDepthBounds = 1.0F,};

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
			VK_COLOR_COMPONENT_A_BIT,});

	myGraphicsState.colorBlend = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.pNext = nullptr,
		.flags = 0,
		.logicOpEnable = VK_FALSE,
		.logicOp = VK_LOGIC_OP_COPY,
		.attachmentCount = static_cast<uint32_t>(myGraphicsState.colorBlendAttachments.size()),
		.pAttachments = myGraphicsState.colorBlendAttachments.data(),
		.blendConstants = {0.0F, 0.0F, 0.0F, 0.0F},};

	myGraphicsState.dynamicStateDescs.clear();
	myGraphicsState.dynamicStateDescs.emplace_back(VK_DYNAMIC_STATE_VIEWPORT);
	myGraphicsState.dynamicStateDescs.emplace_back(VK_DYNAMIC_STATE_SCISSOR);
	// per draw (e.g. per material, for double sided ones), see CommandEncoder::SetCullMode
	myGraphicsState.dynamicStateDescs.emplace_back(VK_DYNAMIC_STATE_CULL_MODE_EXT);
	myGraphicsState.dynamicStateDescs.emplace_back(VK_DYNAMIC_STATE_FRONT_FACE_EXT);
	myGraphicsState.dynamicStateDescs.emplace_back(VK_DYNAMIC_STATE_STENCIL_REFERENCE);

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

// a copy of the graphics state, its pointers aimed at the copy's own storage (so never copied or moved itself), and the
// layout and render pass it is for: what a pipeline is created from, on any thread
template <>
struct Pipeline<kVk>::GraphicsSnapshot
{
	GraphicsState state;
	PipelineLayoutHandle<kVk> layout{};
	RenderPassHandle<kVk> renderPass{};
	std::vector<VkFormat> colorFormats;
	std::string name;

	GraphicsSnapshot(const GraphicsState& source, PipelineLayoutHandle<kVk> layoutHandle, RenderPassHandle<kVk> pass, std::string pipelineName)
		: state(source)
		, layout(layoutHandle)
		, renderPass(pass)
		, name(std::move(pipelineName))
	{
		state.viewport.pViewports = state.viewports.data();
		state.viewport.pScissors = state.scissorRects.data();
		state.dynamicState.pDynamicStates = state.dynamicStateDescs.data();
		state.colorBlend.pAttachments = state.colorBlendAttachments.data();
		if (state.dynamicRendering)
		{
			const auto& rendering = *state.dynamicRendering;
			colorFormats.assign(rendering.pColorAttachmentFormats, rendering.pColorAttachmentFormats + rendering.colorAttachmentCount);
			state.dynamicRendering->pColorAttachmentFormats = colorFormats.data();
		}
	}
	GraphicsSnapshot(const GraphicsSnapshot&) = delete;
	GraphicsSnapshot& operator=(const GraphicsSnapshot&) = delete;
};

template <>
std::shared_ptr<const Pipeline<kVk>::GraphicsSnapshot> Pipeline<kVk>::InternalSnapshotGraphicsState() const
{
	const auto layoutIt = InternalGetLayout();
	ENSURE(layoutIt != myPipelineLayouts.end());
	return std::make_shared<const GraphicsSnapshot>(
		myGraphicsState, static_cast<PipelineLayoutHandle<kVk>>(*layoutIt), std::get<0>(myRenderTarget), GetName());
}

template <>
PipelineHandle<kVk> Pipeline<kVk>::InternalCreateGraphicsPipeline(
	uint64_t hashKey, GraphicsPipelineVariant variant, const GraphicsSnapshot& snapshot) const
{
	ZoneScopedN("Pipeline::InternalCreateGraphicsPipeline");

	const auto& graphicsState = snapshot.state;

	VkGraphicsPipelineCreateInfo pipelineInfo{.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
	pipelineInfo.pNext = graphicsState.dynamicRendering.has_value() ? &graphicsState.dynamicRendering.value() : nullptr;
	pipelineInfo.flags = 0;
	// the layout's stages, with only the variant's fragment stage of them
	std::vector<PipelineShaderStageCreateInfo<kVk>> stages;
	uint32_t fragmentIt = 0;
	bool fragment = false;
	// the variant's specialization constant 0, for every stage (one without it ignores it)
	uint32_t specializationValue = variant.specialization;
	VkSpecializationMapEntry specializationEntry{.constantID = 0, .offset = 0, .size = sizeof(specializationValue)};
	VkSpecializationInfo specialization{
		.mapEntryCount = 1, .pMapEntries = &specializationEntry, .dataSize = sizeof(specializationValue), .pData = &specializationValue};
	for (const auto& stage : graphicsState.shaderStages)
	{
		if (stage.stage == VK_SHADER_STAGE_FRAGMENT_BIT)
		{
			if (fragmentIt++ != variant.fragmentShader)
				continue;
			fragment = true;
		}
		stages.push_back(stage);
		stages.back().pSpecializationInfo = &specialization;
	}
	ENSUREF(fragment || fragmentIt == 0, "the layout has no fragment entry point {}", variant.fragmentShader);
	pipelineInfo.stageCount = static_cast<uint32_t>(stages.size());
	pipelineInfo.pStages = stages.data();
	pipelineInfo.pVertexInputState = &graphicsState.vertexInput;
	auto inputAssembly = graphicsState.inputAssembly;
	inputAssembly.topology = vk::ToVk(variant.topology);
	pipelineInfo.pInputAssemblyState = &inputAssembly;
	pipelineInfo.pViewportState = &graphicsState.viewport;
	pipelineInfo.pRasterizationState = &graphicsState.rasterization;
	pipelineInfo.pMultisampleState = &graphicsState.multisample;
	// a blend state per color attachment of the render target, by the variant's blend modes
	auto depthStencil = graphicsState.depthStencil;
	auto colorAttachmentCount = graphicsState.dynamicRendering ? graphicsState.dynamicRendering->colorAttachmentCount : 1U;
	ENSURE(colorAttachmentCount <= kMaxColorAttachments);
	std::vector<PipelineColorBlendAttachmentState<kVk>> colorBlendAttachments(
		colorAttachmentCount, graphicsState.colorBlendAttachments.front());
	depthStencil.depthWriteEnable = VK_FALSE;
	for (uint32_t attachmentIt = 0; attachmentIt < colorAttachmentCount; attachmentIt++)
	{
		auto& attachment = colorBlendAttachments[attachmentIt];
		auto factors = [&attachment](VkBlendFactor srcColor, VkBlendFactor dstColor, VkBlendFactor srcAlpha, VkBlendFactor dstAlpha)
		{
			attachment.blendEnable = VK_TRUE;
			attachment.srcColorBlendFactor = srcColor;
			attachment.dstColorBlendFactor = dstColor;
			attachment.colorBlendOp = VK_BLEND_OP_ADD;
			attachment.srcAlphaBlendFactor = srcAlpha;
			attachment.dstAlphaBlendFactor = dstAlpha;
			attachment.alphaBlendOp = VK_BLEND_OP_ADD;
		};
		switch (variant.blend[attachmentIt])
		{
		case BlendMode::kOpaque: depthStencil.depthWriteEnable = VK_TRUE; break;
		case BlendMode::kAlpha:
			factors(VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
			break;
		case BlendMode::kNone: attachment.colorWriteMask = 0; break;
		case BlendMode::kAdd: factors(VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE); break;
		case BlendMode::kMultiplyInverse:
			factors(VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
			break;
		}
	}
	// depth only (e.g. shadows): the first blend mode says whether it writes depth
	if (colorAttachmentCount == 0 && variant.blend[0] == BlendMode::kOpaque)
		depthStencil.depthWriteEnable = VK_TRUE;
	// over a pre-pass's depth: read only
	if (variant.depthTest == DepthTest::kEqual)
	{
		depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
		depthStencil.depthWriteEnable = VK_FALSE;
	}
	depthStencil.stencilTestEnable = depthStencil.depthWriteEnable;
	auto colorBlend = graphicsState.colorBlend;
	colorBlend.attachmentCount = colorAttachmentCount;
	colorBlend.pAttachments = colorBlendAttachments.data();
	pipelineInfo.pDepthStencilState = &depthStencil;
	pipelineInfo.pColorBlendState = &colorBlend;
	pipelineInfo.pDynamicState = &graphicsState.dynamicState;
	pipelineInfo.layout = snapshot.layout;
	pipelineInfo.renderPass = snapshot.renderPass;
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

	Track(GetDevice(), VK_OBJECT_TYPE_PIPELINE, pipelineHandle, std::format("{} Graphics Pipeline {}", snapshot.name, hashKey));

	return pipelineHandle;
}

template <>
PipelineHandle<kVk> Pipeline<kVk>::InternalCreateComputePipeline(uint64_t hashKey, ComputePipelineVariant variant)
{
	ZoneScopedN("Pipeline::InternalCreateComputePipeline");

	const auto layoutIt = InternalGetLayout();
	ENSURE(layoutIt != myPipelineLayouts.end());
	const auto& layout = *layoutIt;

	VkComputePipelineCreateInfo pipelineInfo{.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .pNext=nullptr, .flags=0};
	ENSUREF(
		variant.entryPoint < myComputeState.shaderStages.size(),
		"the layout has no compute entry point {} (it has {})",
		variant.entryPoint,
		myComputeState.shaderStages.size());
	pipelineInfo.stage = myComputeState.shaderStages[variant.entryPoint];
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
PipelineHandle<kVk> Pipeline<kVk>::InternalGetPipeline(GraphicsPipelineVariant variant, ComputePipelineVariant computeVariant)
{
	ZoneScopedN("Pipeline::InternalGetPipeline");

	auto key = InternalCalculateHashKey(variant, computeVariant);
	auto& sync = *myPipelineMapSync;
	std::unique_lock lock(sync.mutex);
	auto [it, inserted] = myPipelineMap.try_emplace(key, PipelineHandle<kVk>{});
	if (!inserted)
	{
		// created, or being created (here, or by a job from RequestGraphicsPipeline): wait for it
		if (it->second == nullptr)
		{
			ZoneScopedN("Pipeline::InternalGetPipeline::wait");
			CrashBusy busy; // another thread compiles it (see CrashBusy)
			sync.created.wait(lock, [this, key] { return myPipelineMap.at(key) != nullptr; });
		}
		return myPipelineMap.at(key);
	}

	// create it, without holding the lock (others asking for it wait above)
	lock.unlock();
	PipelineHandle<kVk> handle{};
	{
		ZoneScopedN("Pipeline::InternalGetPipeline::create");
		CrashBusy busy; // inside the driver's compile (see CrashBusy)
		switch (myBindPoint)
		{
		case PipelineBindPoint::kGraphics:
			handle = InternalCreateGraphicsPipeline(key, variant, *InternalSnapshotGraphicsState());
			break;
		case PipelineBindPoint::kCompute:
			handle = InternalCreateComputePipeline(key, computeVariant);
			break;
		default:
			ASSERTF(false, "Not implemented");
		}
	}
	// the draw thread made progress (CrashWatch): after a shader change, its first frames create the GI's compute pipelines
	// cold, which KosmicKrisp compiles in Metal, over 20 s in all, and the watch reported a hang at startup (here, where the
	// draw thread binds, not in the background jobs of RequestGraphicsPipeline, whose progress isn't the draw thread's)
	CrashHeartbeat();
	lock.lock();
	myPipelineMap.at(key) = handle;
	sync.created.notify_all();
	return handle;
}

template <>
Pipeline<kVk>::GraphicsPipelineRequest Pipeline<kVk>::RequestGraphicsPipeline(GraphicsPipelineVariant variant)
{
	ZoneScopedN("Pipeline::RequestGraphicsPipeline");

	ENSURE(myBindPoint == PipelineBindPoint::kGraphics);
	auto key = InternalCalculateHashKey(variant);
	auto& sync = *myPipelineMapSync;
	{
		std::unique_lock lock(sync.mutex);
		auto [it, inserted] = myPipelineMap.try_emplace(key, PipelineHandle<kVk>{});
		if (!inserted)
			return {.handle = it->second};
	}

	// the first ask: a task creating it from the state as it is now
	return {
		.handle = {},
		.create = core::CreateTask(
			[this, key, variant, snapshot = InternalSnapshotGraphicsState()]
			{
				auto handle = InternalCreateGraphicsPipeline(key, variant, *snapshot);
				auto& sync = *myPipelineMapSync;
				std::unique_lock lock(sync.mutex);
				myPipelineMap.at(key) = handle;
				sync.created.notify_all();
			}).handle};
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
PipelineHandle<kVk> Pipeline<kVk>::BindPipelineAuto(CommandBufferHandle<kVk> cmd, ComputePipelineVariant variant)
{
	ENSURE(myBindPoint == PipelineBindPoint::kCompute);
	auto* handle = InternalGetPipeline({}, variant);

	BindPipeline(cmd, myBindPoint, handle);

	return handle;
}

template <>
const ComputeLaunchParameters& Pipeline<kVk>::GetComputeLaunchParameters(ComputePipelineVariant variant) const
{
	ENSURE(myBindPoint == PipelineBindPoint::kCompute && variant.entryPoint < myComputeState.launchParameters.size());
	return myComputeState.launchParameters[variant.entryPoint];
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
PipelineLayoutHandle<kVk> Pipeline<kVk>::CreateLayout(std::string_view name, const ShaderSet& shaderSet)
{
	ENSUREF(!myLayoutNames.contains(std::string(name)), "pipeline layout {} exists", name);

	const auto& [layoutIt, wasInserted] = myPipelineLayouts.emplace(
		PipelineLayout<kVk>(
			PipelineLayoutCreateDesc<kVk>{
				SuperType::CreateDeviceObjectCreateDesc(name),
			},
			shaderSet));

	auto handle = static_cast<PipelineLayoutHandle<kVk>>(*layoutIt);
	myLayoutNames.emplace(std::string(name), handle);
	return handle;
}

template <>
PipelineLayoutHandle<kVk> Pipeline<kVk>::GetLayoutHandle(std::string_view name) const
{
	auto nameIt = myLayoutNames.find(std::string(name));
	ENSUREF(nameIt != myLayoutNames.end(), "no pipeline layout {}", name);
	return nameIt->second;
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
					.pSpecializationInfo=nullptr,});

				myGraphicsState.shaderStageFlags |= vk::ToVk(shaderStage);
			}
		}
		break;
	case PipelineBindPoint::kCompute:
		// every compute entry point, in the layout's order (see ComputePipelineVariant)
		myComputeState.shaderStages.clear();
		myComputeState.launchParameters.clear();
		for (const auto& shader : shaderModules)
		{
			const auto& [entryPointName, shaderStage, launchParams] = shader.GetEntryPoint();
			if (shaderStage != ShaderStage::kCompute)
				continue;
			myComputeState.shaderStages.emplace_back(PipelineShaderStageCreateInfo<kVk>{
				.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
				.pNext = nullptr,
				.flags = 0,
				.stage = VK_SHADER_STAGE_COMPUTE_BIT,
				.module = shader,
				.pName = entryPointName.c_str(),
				.pSpecializationInfo = nullptr,});
			myComputeState.launchParameters.push_back(launchParams.value_or(ComputeLaunchParameters{}));
		}
		ENSUREF(!myComputeState.shaderStages.empty(), "the layout has no compute entry point");
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
	std::swap(myPipelineMapSync, rhs.myPipelineMapSync);
	std::swap(myCache, rhs.myCache);
	std::swap(myBindPoint, rhs.myBindPoint);
	std::swap(myRenderTarget, rhs.myRenderTarget);
	std::swap(myPipelineLayouts, rhs.myPipelineLayouts);
	std::swap(myLayoutNames, rhs.myLayoutNames);
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
		std::println("Saved pipeline cache to {}", fileInfo.value().path);
	}
	else
	{
		std::println("Failed to save pipeline cache, error: {}", fileInfo.error().message());
	}

	for (const auto& pipelineIt : myPipelineMap)
	{
		// null: requested (see RequestGraphicsPipeline), but its job never ran
		if (pipelineIt.second == VK_NULL_HANDLE)
			continue;
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
