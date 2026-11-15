// the D3D12 backend's pipelines: compute pipeline states (graphics ones come with the render passes, step 3), each with
// what its binding sets besides (backend.h's PipelineObject: the variant's specialization, D3D12 having no specialization
// constants, a root constant), bound on the command buffer's object for the next dispatch with the push constants; no
// descriptor pool (the device's shader visible heaps, see descriptorset.cpp) and no pipeline cache yet (a pipeline
// library, later)
#include <rhi/d3d12/backend.h>

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

IMPLEMENT_OBJECT_GETINSTANCE(Pipeline<kD3D12>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(Pipeline<kD3D12>);

template <>
uint64_t Pipeline<kD3D12>::InternalCalculateHashKey(GraphicsPipelineVariant variant, ComputePipelineVariant computeVariant) const
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
	auto* layoutHandle = static_cast<PipelineLayoutHandle<kD3D12>>(*layoutIt);
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
void Pipeline<kD3D12>::InternalResetGraphicsState()
{
	myGraphicsState = {};
}

template <>
void Pipeline<kD3D12>::InternalResetComputeState()
{}

// a copy of the graphics state and the name, what a pipeline is made from on any thread
template <>
struct Pipeline<kD3D12>::GraphicsSnapshot
{
	GraphicsState state;
	std::string name;
};

template <>
std::shared_ptr<const Pipeline<kD3D12>::GraphicsSnapshot> Pipeline<kD3D12>::InternalSnapshotGraphicsState() const
{
	return std::make_shared<const GraphicsSnapshot>(GraphicsSnapshot{.state = myGraphicsState, .name = GetName()});
}

template <>
PipelineHandle<kD3D12> Pipeline<kD3D12>::InternalCreateGraphicsPipeline(
	uint64_t /*hashKey*/, GraphicsPipelineVariant /*variant*/, const GraphicsSnapshot& snapshot) const
{
	ENSUREF(false, "{}: the D3D12 backend has no graphics pipelines yet (docs/backends-plan.md, step 3)", snapshot.name);
	return nullptr;
}

template <>
PipelineHandle<kD3D12> Pipeline<kD3D12>::InternalCreateComputePipeline(
	uint64_t /*hashKey*/, ComputePipelineVariant variant, const PipelineShaderStageCreateInfo<kD3D12>& stage, PipelineLayoutHandle<kD3D12> layout)
{
	ZoneScopedN("Pipeline::InternalCreateComputePipeline");

	auto creationStart = std::chrono::steady_clock::now();
	const auto& code = stage.module->code;
	const D3D12_COMPUTE_PIPELINE_STATE_DESC desc{
		.pRootSignature = layout->rootSignature,
		.CS = {.pShaderBytecode = code.data(), .BytecodeLength = code.size()},
		.NodeMask = 0,
		.CachedPSO = {},
		.Flags = D3D12_PIPELINE_STATE_FLAG_NONE,
	};
	ID3D12PipelineState* state = nullptr;
	d3d12::Check(GetDesc().device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&state)), std::format("{}: CreateComputePipelineState", stage.name));
	d3d12::SetName(state, stage.name);
	pipeline::LogCreation("compute", stage.name, variant.specialization, creationStart);

	// the variant's specialization constant 0 (slang's [vk::constant_id(0)], a uniform in the code): the layout's root
	// constant for it, set at the bind where the code has it, as Vulkan's backend sets the constant (its default where 0
	// isn't: the shaders' defaults are 0)
	return new d3d12::PipelineObject{
		.state = state,
		.rootSignature = layout->rootSignature,
		.layout = layout,
		.globals = layout->globals,
		.specialization = variant.specialization,
		.compute = true,
	};
}

// (the pipelines' map, requests and binds, the launch parameters and the layouts by name are every backend's, in
// pipeline.inl)

template <>
void Pipeline<kD3D12>::BindPipeline(CommandBufferHandle<kD3D12> cmd, PipelineBindPoint /*bindPoint*/, PipelineHandle<kD3D12> handle) const
{
	// set on the list at the next dispatch or draw (see CommandBufferObject::PrepareDispatch)
	cmd->pipeline = handle;
	cmd->layout = handle->layout;
}

template <>
void Pipeline<kD3D12>::PushConstants(CommandBufferHandle<kD3D12> cmd, std::span<const std::byte> data, uint32_t offset) const
{
	const auto layoutIt = InternalGetLayout();
	ENSURE(layoutIt != myPipelineLayouts.end());

	const auto size = static_cast<uint32_t>(data.size());
	ENSUREF(
		std::ranges::any_of(
			layoutIt->GetPushConstantRanges(),
			[offset, size](const auto& range) { return range.offset < offset + size && offset < range.offset + range.size; }),
		"No push constant range in the current layout covers the pushed data.");
	ENSUREF(offset % 4 == 0, "push constants at offset {}: root constants are 32 bit values", offset);

	// set as root constants at the next dispatch or draw (the root signature's whole range: what wasn't pushed is zero)
	const auto* layout = static_cast<PipelineLayoutHandle<kD3D12>>(*layoutIt);
	if (cmd->pushConstants.size() < layout->pushConstantCount)
		cmd->pushConstants.resize(layout->pushConstantCount);
	std::memcpy(reinterpret_cast<std::byte*>(cmd->pushConstants.data()) + offset, data.data(), size);
}

template <>
void Pipeline<kD3D12>::BindLayoutAuto(PipelineLayoutHandle<kD3D12> layoutHandle, PipelineBindPoint bindPoint)
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
void Pipeline<kD3D12>::SetRenderTarget(IRenderTarget<kD3D12>& renderTarget)
{
	myGraphicsState.rendering = renderTarget.GetPipelineRenderingCreateInfo();
	myGraphicsState.extent = renderTarget.GetExtent();
}

template <>
Pipeline<kD3D12>::Pipeline(CreateDescType&& desc)
	: DeviceObject(std::forward<CreateDescType>(desc))
{
	InternalResetGraphicsState();
	InternalResetComputeState();
}

// (Swap and the moves are every backend's, in pipeline.inl)

template <>
Pipeline<kD3D12>::~Pipeline()
{
	if (!IsValid())
		return;

	for (const auto& [key, handle] : myPipelineMap)
	{
		// null: requested (see RequestGraphicsPipeline), but its job never ran
		if (handle == nullptr)
			continue;
		d3d12::Release(handle->state);
		delete handle;
	}

	myDescriptorMap.clear();
}

} // namespace rhi
