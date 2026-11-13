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

	// (a graphics pipeline's render target formats come with the render passes, step 5)
	if (myBindPoint == PipelineBindPoint::kGraphics)
		result = XXH3_64bits_update(gThreadXxhState.get(), &variant, sizeof(variant));
	else
		result = XXH3_64bits_update(gThreadXxhState.get(), &computeVariant, sizeof(computeVariant));
	ENSURE(result != XXH_ERROR);

	return XXH3_64bits_digest(gThreadXxhState.get());
}

template <>
void Pipeline<kMetal>::InternalResetGraphicsState()
{
	// (the graphics state comes with the render passes, step 5)
}

template <>
void Pipeline<kMetal>::InternalResetComputeState()
{}

template <>
struct Pipeline<kMetal>::GraphicsSnapshot
{
	std::string name;
};

template <>
std::shared_ptr<const Pipeline<kMetal>::GraphicsSnapshot> Pipeline<kMetal>::InternalSnapshotGraphicsState() const
{
	return std::make_shared<const GraphicsSnapshot>(GraphicsSnapshot{.name = GetName()});
}

template <>
PipelineHandle<kMetal> Pipeline<kMetal>::InternalCreateGraphicsPipeline(
	uint64_t /*hashKey*/, GraphicsPipelineVariant /*variant*/, const GraphicsSnapshot& snapshot) const
{
	ENSUREF(false, "{}: the Metal backend's graphics pipelines come with its render passes (docs/backends-plan.md, step 5)", snapshot.name);
	return nullptr;
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
	MTL::Function* function = stage.library->newFunction(NS::String::string(stage.name, NS::UTF8StringEncoding), constants, &error);
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

	return new metal::PipelineObject{.computePipeline = state};
}

// (the pipelines' map, requests and binds, the launch parameters and the layouts by name are every backend's, in
// pipeline.inl)

template <>
void Pipeline<kMetal>::BindPipeline(CommandBufferHandle<kMetal> cmd, PipelineBindPoint bindPoint, PipelineHandle<kMetal> handle) const
{
	ENSUREF(bindPoint == PipelineBindPoint::kCompute, "the Metal backend's graphics pipelines come with its render passes (step 5)");
	cmd->computePipeline = handle->computePipeline;
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

	// set at the layout's slot for them at the next dispatch (see commandencoder.cpp)
	cmd->pushConstantSlot = static_cast<PipelineLayoutHandle<kMetal>>(*layoutIt)->pushConstantSlot;
	if (cmd->pushConstants.size() < offset + size)
		cmd->pushConstants.resize(offset + size);
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
				myGraphicsState.shaderStages.push_back({.library = shader, .name = entryPointName.c_str(), .stage = shaderStage});
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
			myComputeState.shaderStages.push_back({.library = shader, .name = entryPointName.c_str(), .stage = shaderStage});
			myComputeState.launchParameters.push_back(launchParams.value_or(ComputeLaunchParameters{}));
		}
		ENSUREF(!myComputeState.shaderStages.empty(), "the layout has no compute entry point");
		break;
	}

	InternalPrepareDescriptorSets();
}

template <>
void Pipeline<kMetal>::SetRenderTarget(IRenderTarget<kMetal>& /*renderTarget*/)
{
	ENSUREF(false, "the Metal backend's render targets come with its render passes (docs/backends-plan.md, step 5)");
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
		handle->computePipeline->release();
		delete handle;
	}

	myDescriptorMap.clear();
}

} // namespace rhi
