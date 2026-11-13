// the Metal backend's pipelines: compute pipeline states, each from its entry point's library with the variant's
// specialization as function constant 0 (graphics ones come with the render passes, step 5), bound on the command
// buffer's object for the next dispatch with the push constants; no descriptor pool (an array of sets is a buffer of its
// own, see descriptorset.cpp) and no pipeline cache yet (binary archives, later)
#include <rhi/metal/backend.h>

#include <rhi/pipeline.h>
#include <rhi/rhi.h>

#include <core/assert.h>
#include <core/profiling.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>

namespace rhi
{

IMPLEMENT_OBJECT_GETINSTANCE(Pipeline<kMetal>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(Pipeline<kMetal>);

template <>
uint64_t Pipeline<kMetal>::InternalCalculateHashKey(GraphicsPipelineVariant variant, ComputePipelineVariant computeVariant) const
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
	auto* layoutHandle = static_cast<PipelineLayoutHandle<kMetal>>(*layoutIt);
	result = XXH3_64bits_update(gThreadXxhState.get(), &layoutHandle, sizeof(layoutHandle));
	ENSURE(result != XXH_ERROR);

	if (myBindPoint == PipelineBindPoint::kGraphics)
	{
		result = XXH3_64bits_update(gThreadXxhState.get(), &variant, sizeof(variant));
		ENSURE(result != XXH_ERROR);
		// and the render target's formats: a variant needs a pipeline per render target layout, as Vulkan's backend has it
		if (const auto& rendering = myGraphicsState.rendering)
			result = XXH3_64bits_update(gThreadXxhState.get(), &*rendering, sizeof(*rendering));
	}
	else
	{
		result = XXH3_64bits_update(gThreadXxhState.get(), &computeVariant, sizeof(computeVariant));
	}
	ENSURE(result != XXH_ERROR);

	return XXH3_64bits_digest(gThreadXxhState.get());
}

template <>
void Pipeline<kMetal>::InternalResetGraphicsState()
{
	myGraphicsState = {};
}

template <>
void Pipeline<kMetal>::InternalResetComputeState()
{}

// a copy of the graphics state and the name, what a pipeline is made from on any thread
template <>
struct Pipeline<kMetal>::GraphicsSnapshot
{
	GraphicsState state;
	std::string name;
};

template <>
std::shared_ptr<const Pipeline<kMetal>::GraphicsSnapshot> Pipeline<kMetal>::InternalSnapshotGraphicsState() const
{
	return std::make_shared<const GraphicsSnapshot>(GraphicsSnapshot{.state = myGraphicsState, .name = GetName()});
}

namespace pipeline
{

// an entry point's function, with the variant's specialization as function constant 0 where it isn't 0 (else the shader's
// default, as Vulkan's backend has it), released by the caller
static MTL::Function* NewFunction(const PipelineShaderStageCreateInfo<kMetal>& stage, uint32_t specialization)
{
	auto* constants = MTL::FunctionConstantValues::alloc()->init();
	if (specialization != 0)
		constants->setConstantValue(&specialization, MTL::DataTypeUInt, NS::UInteger{0});
	NS::Error* error = nullptr;
	MTL::Function* function =
		stage.module->Library()->newFunction(NS::String::string(stage.name, NS::UTF8StringEncoding), constants, &error);
	constants->release();
	ENSUREF(
		function != nullptr,
		"Failed to make {}'s Metal function: {}",
		stage.name,
		error != nullptr ? error->localizedDescription()->utf8String() : "(no error)");
	return function;
}

// a color attachment's blending for a blend mode (Vulkan's backend's factors); false where it writes depth (opaque)
static bool SetBlend(MTL::RenderPipelineColorAttachmentDescriptor& attachment, BlendMode mode)
{
	auto factors = [&attachment](MTL::BlendFactor srcColor, MTL::BlendFactor dstColor, MTL::BlendFactor srcAlpha, MTL::BlendFactor dstAlpha)
	{
		attachment.setBlendingEnabled(true);
		attachment.setSourceRGBBlendFactor(srcColor);
		attachment.setDestinationRGBBlendFactor(dstColor);
		attachment.setRgbBlendOperation(MTL::BlendOperationAdd);
		attachment.setSourceAlphaBlendFactor(srcAlpha);
		attachment.setDestinationAlphaBlendFactor(dstAlpha);
		attachment.setAlphaBlendOperation(MTL::BlendOperationAdd);
	};
	switch (mode)
	{
	case BlendMode::kOpaque: return true;
	case BlendMode::kAlpha:
		factors(MTL::BlendFactorSourceAlpha, MTL::BlendFactorOneMinusSourceAlpha, MTL::BlendFactorOne, MTL::BlendFactorOneMinusSourceAlpha);
		break;
	case BlendMode::kNone: attachment.setWriteMask(MTL::ColorWriteMaskNone); break;
	case BlendMode::kAdd: factors(MTL::BlendFactorOne, MTL::BlendFactorOne, MTL::BlendFactorOne, MTL::BlendFactorOne); break;
	case BlendMode::kMultiplyInverse:
		factors(MTL::BlendFactorZero, MTL::BlendFactorOneMinusSourceColor, MTL::BlendFactorZero, MTL::BlendFactorOneMinusSourceAlpha);
		break;
	case BlendMode::kMultiplyMax:
		factors(MTL::BlendFactorDestinationColor, MTL::BlendFactorZero, MTL::BlendFactorOne, MTL::BlendFactorOne);
		attachment.setAlphaBlendOperation(MTL::BlendOperationMax);
		break;
	}
	return false;
}

} // namespace pipeline

template <>
PipelineHandle<kMetal> Pipeline<kMetal>::InternalCreateGraphicsPipeline(
	uint64_t /*hashKey*/, GraphicsPipelineVariant variant, const GraphicsSnapshot& snapshot) const
{
	ZoneScopedN("Pipeline::InternalCreateGraphicsPipeline");

	const auto& state = snapshot.state;
	ENSUREF(state.rendering, "{}: a graphics pipeline without a render target (Pipeline::SetRenderTarget)", snapshot.name);
	const auto& rendering = *state.rendering;

	// the layout's vertex stage, and the variant's fragment stage of its fragment stages
	const PipelineShaderStageCreateInfo<kMetal>* vertex = nullptr;
	const PipelineShaderStageCreateInfo<kMetal>* fragment = nullptr;
	uint32_t fragmentIt = 0;
	for (const auto& stage : state.shaderStages)
	{
		if (stage.stage == ShaderStage::kVertex)
			vertex = &stage;
		else if (stage.stage == ShaderStage::kFragment && fragmentIt++ == variant.fragmentShader)
			fragment = &stage;
	}
	ENSUREF(vertex != nullptr, "{}: the layout has no vertex entry point", snapshot.name);
	ENSUREF(fragment != nullptr || fragmentIt == 0, "{}: the layout has no fragment entry point {}", snapshot.name, variant.fragmentShader);

	auto creationStart = std::chrono::steady_clock::now();
	auto* descriptor = MTL::RenderPipelineDescriptor::alloc()->init();
	auto* vertexFunction = pipeline::NewFunction(*vertex, variant.specialization);
	descriptor->setVertexFunction(vertexFunction);
	vertexFunction->release();
	if (fragment != nullptr)
	{
		auto* fragmentFunction = pipeline::NewFunction(*fragment, variant.specialization);
		descriptor->setFragmentFunction(fragmentFunction);
		fragmentFunction->release();
	}
	// a blend state per color attachment by the variant's blend modes; depth written where one is opaque (a depth only
	// target: where the first is)
	bool depthWrite = rendering.colorAttachmentCount == 0 && variant.blend[0] == BlendMode::kOpaque;
	for (uint32_t attachmentIt = 0; attachmentIt < rendering.colorAttachmentCount; attachmentIt++)
	{
		auto* attachment = descriptor->colorAttachments()->object(attachmentIt);
		attachment->setPixelFormat(metal::ToMetal(rendering.colorAttachmentFormats[attachmentIt]));
		depthWrite = pipeline::SetBlend(*attachment, variant.blend[attachmentIt]) || depthWrite;
	}
	if (rendering.depthAttachmentFormat != Format::kUndefined)
		descriptor->setDepthAttachmentPixelFormat(metal::ToMetal(rendering.depthAttachmentFormat));
	if (rendering.stencilAttachmentFormat != Format::kUndefined)
		descriptor->setStencilAttachmentPixelFormat(metal::ToMetal(rendering.stencilAttachmentFormat));
	// the draws' primitives; the topology class only for points: the vertex stage writes the point size for every
	// topology (VertexMain: Vulkan needs it for point lists), which Metal refuses in a pipeline of the triangle or line
	// class, not one of none (Metal's default, which only layered rendering needs named)
	auto* object = new metal::PipelineObject{};
	switch (variant.topology)
	{
	case PrimitiveTopology::kTriangleList:
		object->primitiveType = MTL::PrimitiveTypeTriangle;
		break;
	case PrimitiveTopology::kLineList:
		object->primitiveType = MTL::PrimitiveTypeLine;
		break;
	case PrimitiveTopology::kPointList:
		descriptor->setInputPrimitiveTopology(MTL::PrimitiveTopologyClassPoint);
		object->primitiveType = MTL::PrimitiveTypePoint;
		break;
	}
	descriptor->setLabel(NS::String::string(snapshot.name.c_str(), NS::UTF8StringEncoding));
	NS::Error* error = nullptr;
	object->renderPipeline = GetDesc().device->newRenderPipelineState(descriptor, &error);
	descriptor->release();
	ENSUREF(
		object->renderPipeline != nullptr,
		"{}: Failed to make a Metal render pipeline: {}",
		snapshot.name,
		error != nullptr ? error->localizedDescription()->utf8String() : "(no error)");

	// the depth test, reversed (1 at the near plane; over a pre-pass's depth, equal and read only), and what writes depth
	// writes the stencil's first bit too (the reference is the encoder's, CommandEncoder::SetStencilReference)
	if (rendering.depthAttachmentFormat != Format::kUndefined)
	{
		auto* depthStencil = MTL::DepthStencilDescriptor::alloc()->init();
		const bool equal = variant.depthTest == DepthTest::kEqual;
		depthStencil->setDepthCompareFunction(equal ? MTL::CompareFunctionGreaterEqual : MTL::CompareFunctionGreater);
		depthStencil->setDepthWriteEnabled(depthWrite && !equal);
		if (rendering.stencilAttachmentFormat != Format::kUndefined && depthWrite && !equal)
		{
			auto* stencil = MTL::StencilDescriptor::alloc()->init();
			stencil->setStencilCompareFunction(MTL::CompareFunctionAlways);
			stencil->setStencilFailureOperation(MTL::StencilOperationKeep);
			stencil->setDepthFailureOperation(MTL::StencilOperationKeep);
			stencil->setDepthStencilPassOperation(MTL::StencilOperationReplace);
			stencil->setReadMask(0xFFU);
			stencil->setWriteMask(1U);
			depthStencil->setFrontFaceStencil(stencil);
			depthStencil->setBackFaceStencil(stencil);
			stencil->release();
		}
		object->depthStencil = GetDesc().device->newDepthStencilState(depthStencil);
		depthStencil->release();
	}
	pipeline::LogCreation("graphics", fragment != nullptr ? fragment->name : "(no fragment stage)", variant.specialization, creationStart);

	return object;
}

template <>
PipelineHandle<kMetal> Pipeline<kMetal>::InternalCreateComputePipeline(
	uint64_t /*hashKey*/, ComputePipelineVariant variant, const PipelineShaderStageCreateInfo<kMetal>& stage, PipelineLayoutHandle<kMetal> /*layout*/)
{
	ZoneScopedN("Pipeline::InternalCreateComputePipeline");

	auto creationStart = std::chrono::steady_clock::now();
	// the variant's specialization constant 0 (slang's [vk::constant_id(0)], function constant 0 in its MSL), where it has
	// one, as Vulkan's backend sets it: otherwise the shader's default
	auto* constants = MTL::FunctionConstantValues::alloc()->init();
	uint32_t specializationValue = variant.specialization;
	if (variant.specialization != 0)
		constants->setConstantValue(&specializationValue, MTL::DataTypeUInt, NS::UInteger{0});
	NS::Error* error = nullptr;
	MTL::Function* function =
		stage.module->Library()->newFunction(NS::String::string(stage.name, NS::UTF8StringEncoding), constants, &error);
	constants->release();
	ENSUREF(
		function != nullptr,
		"Failed to make {}'s Metal function: {}",
		stage.name,
		error != nullptr ? error->localizedDescription()->utf8String() : "(no error)");

	MTL::ComputePipelineState* state = GetDesc().device->newComputePipelineState(function, &error);
	function->release();
	ENSUREF(
		state != nullptr,
		"Failed to make {}'s Metal compute pipeline: {}",
		stage.name,
		error != nullptr ? error->localizedDescription()->utf8String() : "(no error)");
	pipeline::LogCreation("compute", stage.name, variant.specialization, creationStart);

	return new metal::PipelineObject{.computePipeline = state, .threadGroupSize = stage.threadGroupSize};
}

// (the pipelines' map, requests and binds, the launch parameters and the layouts by name are every backend's, in
// pipeline.inl)

template <>
void Pipeline<kMetal>::BindPipeline(CommandBufferHandle<kMetal> cmd, PipelineBindPoint bindPoint, PipelineHandle<kMetal> handle) const
{
	// set on the encoder at the next dispatch or draw (see commandencoder.cpp): a render pipeline may be bound before its
	// pass begins
	if (bindPoint == PipelineBindPoint::kCompute)
	{
		cmd->computePipeline = handle->computePipeline;
		cmd->computeThreadGroupSize = handle->threadGroupSize;
		return;
	}
	cmd->renderPipeline = handle->renderPipeline;
	cmd->depthStencil = handle->depthStencil;
	cmd->primitiveType = handle->primitiveType;
}

template <>
void Pipeline<kMetal>::PushConstants(CommandBufferHandle<kMetal> cmd, std::span<const std::byte> data, uint32_t offset) const
{
	const auto layoutIt = InternalGetLayout();
	ENSURE(layoutIt != myPipelineLayouts.end());

	const auto size = static_cast<uint32_t>(data.size());
	ENSUREF(
		std::ranges::any_of(
			layoutIt->GetPushConstantRanges(),
			[offset, size](const auto& range) { return range.offset < offset + size && offset < range.offset + range.size; }),
		"No push constant range in the current layout covers the pushed data.");

	// set at the layout's slot for them at the next dispatch or draw (see commandencoder.cpp), padded with zeros to a
	// multiple of 16 bytes: setBytes must cover the shader's whole struct, which MSL rounds up to its alignment (a float4's
	// 16 at most), where Vulkan's push constants take fewer bytes than the range (rhitest's draw pushes 36 of 48: Metal's
	// debug layer stopped the draw)
	constexpr uint32_t kStructAlignment = 16;
	cmd->pushConstantSlot = static_cast<PipelineLayoutHandle<kMetal>>(*layoutIt)->pushConstantSlot;
	const size_t padded = (static_cast<size_t>(offset + size) + kStructAlignment - 1) & ~size_t{kStructAlignment - 1};
	if (cmd->pushConstants.size() < padded)
		cmd->pushConstants.resize(padded);
	std::memcpy(cmd->pushConstants.data() + offset, data.data(), size);
}

template <>
void Pipeline<kMetal>::BindLayoutAuto(PipelineLayoutHandle<kMetal> layoutHandle, PipelineBindPoint bindPoint)
{
	myBindPoint = bindPoint;
	myCurrentLayoutIt = myPipelineLayouts.find(layoutHandle);
	ENSURE(myCurrentLayoutIt != myPipelineLayouts.end());
	const auto& shaderModules = myCurrentLayoutIt->GetShaderModules();

	ENSURE(!shaderModules.empty());

	switch (myBindPoint)
	{
	case PipelineBindPoint::kGraphics:
		myGraphicsState.shaderStages.clear();
		for (const auto& shader : shaderModules)
		{
			const auto& [entryPointName, shaderStage, launchParams] = shader.GetEntryPoint();
			if (Any(shaderStage & ShaderStage::kAllGraphics))
				myGraphicsState.shaderStages.push_back({.module = shader, .name = entryPointName.c_str(), .stage = shaderStage});
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
			const auto launch = launchParams.value_or(ComputeLaunchParameters{});
			myComputeState.shaderStages.push_back(
				{.module = shader, .name = entryPointName.c_str(), .stage = shaderStage, .threadGroupSize = launch.threadGroupSize});
			myComputeState.launchParameters.push_back(launch);
		}
		ENSUREF(!myComputeState.shaderStages.empty(), "the layout has no compute entry point");
		break;
	}

	InternalPrepareDescriptorSets();
}

template <>
void Pipeline<kMetal>::SetRenderTarget(IRenderTarget<kMetal>& renderTarget)
{
	myGraphicsState.rendering = renderTarget.GetPipelineRenderingCreateInfo();
	myGraphicsState.extent = renderTarget.GetExtent();
}

template <>
Pipeline<kMetal>::Pipeline(CreateDescType&& desc)
	: DeviceObject(std::forward<CreateDescType>(desc))
{
	InternalResetGraphicsState();
	InternalResetComputeState();
}

// (Swap and the moves are every backend's, in pipeline.inl)

template <>
Pipeline<kMetal>::~Pipeline()
{
	if (!IsValid())
		return;

	for (const auto& [key, handle] : myPipelineMap)
	{
		// null: requested (see RequestGraphicsPipeline), but its job never ran
		if (handle == nullptr)
			continue;
		for (NS::Object* object : std::initializer_list<NS::Object*>{handle->computePipeline, handle->renderPipeline, handle->depthStencil})
			if (object != nullptr)
				object->release();
		delete handle;
	}

	myDescriptorMap.clear();
}

} // namespace rhi
