// the D3D12 backend's pipelines: compute and graphics pipeline states, each of its variant's specialization's binary
// (D3D12 has no specialization constants: ShaderModuleObject), bound on the command buffer's object for the next dispatch
// or draw with the push constants; no descriptor pool (the device's shader visible heaps, see descriptorset.cpp); a
// pipeline library for a cache (PipelineCacheObject), saved to the user profile's pipeline.d3d12.cache
#include <rhi/d3d12/backend.h>

#include <rhi/pipeline.h>
#include <rhi/rhi.h>

#include <core/assert.h>
#include <core/file.h>
#include <core/profiling.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <memory>
#include <print>
#include <span>
#include <string>

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

namespace pipeline
{

// a color attachment's blending for a blend mode (Vulkan's backend's factors); true where it writes depth (opaque)
static bool SetBlend(D3D12_RENDER_TARGET_BLEND_DESC& target, BlendMode mode)
{
	target = D3D12_RENDER_TARGET_BLEND_DESC{
		.BlendEnable = FALSE,
		.LogicOpEnable = FALSE,
		.SrcBlend = D3D12_BLEND_ONE,
		.DestBlend = D3D12_BLEND_ZERO,
		.BlendOp = D3D12_BLEND_OP_ADD,
		.SrcBlendAlpha = D3D12_BLEND_ONE,
		.DestBlendAlpha = D3D12_BLEND_ZERO,
		.BlendOpAlpha = D3D12_BLEND_OP_ADD,
		.LogicOp = D3D12_LOGIC_OP_NOOP,
		.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL};
	auto factors = [&target](D3D12_BLEND srcColor, D3D12_BLEND dstColor, D3D12_BLEND srcAlpha, D3D12_BLEND dstAlpha)
	{
		target.BlendEnable = TRUE;
		target.SrcBlend = srcColor;
		target.DestBlend = dstColor;
		target.SrcBlendAlpha = srcAlpha;
		target.DestBlendAlpha = dstAlpha;
	};
	switch (mode)
	{
	case BlendMode::kOpaque: return true;
	case BlendMode::kAlpha: factors(D3D12_BLEND_SRC_ALPHA, D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_ONE, D3D12_BLEND_INV_SRC_ALPHA); break;
	case BlendMode::kNone: target.RenderTargetWriteMask = 0; break;
	case BlendMode::kAdd: factors(D3D12_BLEND_ONE, D3D12_BLEND_ONE, D3D12_BLEND_ONE, D3D12_BLEND_ONE); break;
	case BlendMode::kMultiplyInverse: factors(D3D12_BLEND_ZERO, D3D12_BLEND_INV_SRC_COLOR, D3D12_BLEND_ZERO, D3D12_BLEND_INV_SRC_ALPHA); break;
	case BlendMode::kMultiplyMax:
		factors(D3D12_BLEND_DEST_COLOR, D3D12_BLEND_ZERO, D3D12_BLEND_ONE, D3D12_BLEND_ONE);
		target.BlendOpAlpha = D3D12_BLEND_OP_MAX;
		break;
	}
	return false;
}

} // namespace pipeline

namespace d3d12
{

ID3D12PipelineState* PipelineObject::State(ID3D12Device* device, CullMode cullMode, FrontFace frontFace)
{
	const auto index = (static_cast<size_t>(cullMode) * 2) + static_cast<size_t>(frontFace);
	std::scoped_lock lock{mutex};
	auto& state = states.at(index);
	if (state == nullptr)
	{
		auto desc = graphics;
		desc.RasterizerState.CullMode =
			cullMode == CullMode::kNone ? D3D12_CULL_MODE_NONE : (cullMode == CullMode::kFront ? D3D12_CULL_MODE_FRONT : D3D12_CULL_MODE_BACK);
		desc.RasterizerState.FrontCounterClockwise = frontFace == FrontFace::kCounterClockwise ? TRUE : FALSE;
		state = PipelineCacheObject::Graphics(cache, device, desc, HashBytesOf(index, key), name);
		SetName(state, name);
	}
	return state;
}

namespace pipelinecache
{

std::wstring NameOf(uint64_t key)
{
	return std::format(L"{:016x}", key);
}

// the library's file: the path given, D3D12's of its own where its name doesn't say so (gfx names the ImGui renderer's
// imgui.pipeline.cache for every api: D3D12 read Vulkan's, a blob it couldn't understand, the debug layer's error 962)
std::filesystem::path PathOf(const std::filesystem::path& path)
{
	if (path.empty() || path.filename().string().contains("d3d12"))
		return path;
	auto result = path;
	result.replace_extension(std::format(".d3d12{}", path.extension().string()));
	return result;
}

// the library of the file's bytes, or an empty one where there are none, or they're another driver's or adapter's
// (D3D12_ERROR_DRIVER_VERSION_MISMATCH, ADAPTER_NOT_FOUND) or not a library's
PipelineCacheObject* Open(ID3D12Device* device, const std::filesystem::path& path)
{
	ID3D12Device1* device1 = nullptr;
	if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device1))))
		return nullptr;
	auto* cache = new PipelineCacheObject{}; //NOLINT(cppcoreguidelines-owning-memory)
	if (auto file = core::file::Map(path); file && file->size() > 0)
		cache->bytes.assign(file->begin(), file->end());
	HRESULT result = cache->bytes.empty() ? E_FAIL
										  : device1->CreatePipelineLibrary(cache->bytes.data(), cache->bytes.size(), IID_PPV_ARGS(&cache->library));
	if (FAILED(result))
	{
		if (!cache->bytes.empty())
			std::println("D3D12: the pipeline cache {} is another driver's or adapter's ({:#010x}): made again", path.string(), static_cast<uint32_t>(result));
		cache->bytes.clear();
		result = device1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&cache->library));
	}
	Release(device1);
	if (FAILED(result))
	{
		std::println(stderr, "D3D12: no pipeline library ({:#010x}): pipelines aren't cached", static_cast<uint32_t>(result));
		delete cache; //NOLINT(cppcoreguidelines-owning-memory)
		return nullptr;
	}
	return cache;
}

// the library written to the file where a state was stored since it was loaded, and released
void Close(PipelineCacheObject* cache, const std::filesystem::path& path)
{
	if (cache == nullptr)
		return;
	if (cache->stored)
	{
		std::vector<std::byte> bytes(cache->library->GetSerializedSize());
		if (SUCCEEDED(cache->library->Serialize(bytes.data(), bytes.size())))
		{
			if (auto written = core::file::Write(path, bytes); written)
				std::println("Saved pipeline cache to {}", path.string());
			else
				std::println("Failed to save pipeline cache, error: {}", written.error().message());
		}
	}
	Release(cache->library);
	delete cache; //NOLINT(cppcoreguidelines-owning-memory)
}

} // namespace pipelinecache

ID3D12PipelineState* PipelineCacheObject::Graphics(
	PipelineCacheObject* cache, ID3D12Device* device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc, uint64_t key, std::string_view name)
{
	ID3D12PipelineState* state = nullptr;
	const auto cachedName = pipelinecache::NameOf(key);
	if (cache != nullptr)
	{
		if (SUCCEEDED(cache->library->LoadGraphicsPipeline(cachedName.c_str(), &desc, IID_PPV_ARGS(&state))))
			return state;
	}
	Check(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&state)), std::format("{}: CreateGraphicsPipelineState", name));
	if (cache != nullptr)
	{
		std::scoped_lock lock{cache->mutex};
		cache->stored = SUCCEEDED(cache->library->StorePipeline(cachedName.c_str(), state)) || cache->stored;
	}
	return state;
}

ID3D12PipelineState* PipelineCacheObject::Compute(
	PipelineCacheObject* cache, ID3D12Device* device, const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc, uint64_t key, std::string_view name)
{
	ID3D12PipelineState* state = nullptr;
	const auto cachedName = pipelinecache::NameOf(key);
	if (cache != nullptr)
	{
		if (SUCCEEDED(cache->library->LoadComputePipeline(cachedName.c_str(), &desc, IID_PPV_ARGS(&state))))
			return state;
	}
	Check(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&state)), std::format("{}: CreateComputePipelineState", name));
	if (cache != nullptr)
	{
		std::scoped_lock lock{cache->mutex};
		cache->stored = SUCCEEDED(cache->library->StorePipeline(cachedName.c_str(), state)) || cache->stored;
	}
	return state;
}

} // namespace d3d12

template <>
PipelineHandle<kD3D12> Pipeline<kD3D12>::InternalCreateGraphicsPipeline(
	uint64_t /*hashKey*/, GraphicsPipelineVariant variant, const GraphicsSnapshot& snapshot) const
{
	ZoneScopedN("Pipeline::InternalCreateGraphicsPipeline");

	const auto& state = snapshot.state;
	ENSUREF(state.rendering, "{}: a graphics pipeline without a render target (Pipeline::SetRenderTarget)", snapshot.name);
	const auto& rendering = *state.rendering;

	// the layout's vertex stage, and the variant's fragment stage of its fragment stages
	const PipelineShaderStageCreateInfo<kD3D12>* vertex = nullptr;
	const PipelineShaderStageCreateInfo<kD3D12>* fragment = nullptr;
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
	// the layout's: the stages' first module's (a layout's modules share its root signature)
	const auto& layouts = myPipelineLayouts;
	const auto layoutIt = InternalGetLayout();
	ENSURE(layoutIt != layouts.end());
	const auto* layout = static_cast<PipelineLayoutHandle<kD3D12>>(*layoutIt);

	auto* object = new d3d12::PipelineObject{};
	object->rootSignature = layout->rootSignature;
	object->layout = layout;
	object->name = fragment != nullptr ? fragment->name : vertex->name;
	auto& desc = object->graphics;
	desc.pRootSignature = layout->rootSignature;
	const auto& vertexCode = vertex->module->Code(variant.specialization);
	desc.VS = {.pShaderBytecode = vertexCode.data(), .BytecodeLength = vertexCode.size()};
	if (fragment != nullptr)
	{
		const auto& fragmentCode = fragment->module->Code(variant.specialization);
		desc.PS = {.pShaderBytecode = fragmentCode.data(), .BytecodeLength = fragmentCode.size()};
	}
	// a blend state per color attachment by the variant's blend modes; depth written where one is opaque (a depth only
	// target: where the first is)
	desc.BlendState.IndependentBlendEnable = TRUE;
	bool depthWrite = rendering.colorAttachmentCount == 0 && variant.blend[0] == BlendMode::kOpaque;
	desc.NumRenderTargets = rendering.colorAttachmentCount;
	for (uint32_t attachmentIt = 0; attachmentIt < rendering.colorAttachmentCount; attachmentIt++)
	{
		desc.RTVFormats[attachmentIt] = d3d12::ToD3D12(rendering.colorAttachmentFormats[attachmentIt]);
		depthWrite = pipeline::SetBlend(desc.BlendState.RenderTarget[attachmentIt], variant.blend[attachmentIt]) || depthWrite;
	}
	desc.SampleMask = UINT_MAX;
	desc.RasterizerState = D3D12_RASTERIZER_DESC{
		.FillMode = D3D12_FILL_MODE_SOLID,
		.CullMode = D3D12_CULL_MODE_NONE,
		.FrontCounterClockwise = TRUE,
		.DepthBias = 0,
		.DepthBiasClamp = 0.0F,
		.SlopeScaledDepthBias = 0.0F,
		.DepthClipEnable = TRUE,
		.MultisampleEnable = FALSE,
		.AntialiasedLineEnable = FALSE,
		.ForcedSampleCount = 0,
		.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF};
	// the depth test, reversed (1 at the near plane; over a pre-pass's depth, equal and read only), and what writes depth
	// writes the stencil's first bit too (the reference is the list's, CommandEncoder::SetStencilReference)
	if (rendering.depthAttachmentFormat != Format::kUndefined)
	{
		const bool equal = variant.depthTest == DepthTest::kEqual;
		desc.DSVFormat = d3d12::ToD3D12(rendering.depthAttachmentFormat);
		desc.DepthStencilState.DepthEnable = TRUE;
		desc.DepthStencilState.DepthFunc = equal ? D3D12_COMPARISON_FUNC_GREATER_EQUAL : D3D12_COMPARISON_FUNC_GREATER;
		desc.DepthStencilState.DepthWriteMask = depthWrite && !equal ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
		if (rendering.stencilAttachmentFormat != Format::kUndefined && depthWrite && !equal)
		{
			const D3D12_DEPTH_STENCILOP_DESC replace{
				.StencilFailOp = D3D12_STENCIL_OP_KEEP,
				.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP,
				.StencilPassOp = D3D12_STENCIL_OP_REPLACE,
				.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS};
			desc.DepthStencilState.StencilEnable = TRUE;
			desc.DepthStencilState.StencilReadMask = 0xFF;
			desc.DepthStencilState.StencilWriteMask = 1;
			desc.DepthStencilState.FrontFace = replace;
			desc.DepthStencilState.BackFace = replace;
		}
	}
	switch (variant.topology)
	{
	case PrimitiveTopology::kTriangleList:
		desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		object->topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		break;
	case PrimitiveTopology::kLineList:
		desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
		object->topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
		break;
	case PrimitiveTopology::kPointList:
		desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
		object->topology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
		break;
	}
	desc.SampleDesc = {.Count = 1, .Quality = 0};
	// its name in the cache: its shaders' bytecode, its root signature's, and its fixed state, field by field (the
	// desc's padding, the blend's and the depth stencil's, needn't be zeros: hashed whole, the names changed from run to
	// run)
	object->cache = myCache;
	{
		using d3d12::HashBytesOf;
		uint64_t key = d3d12::HashBytes(std::as_bytes(std::span(vertexCode)));
		if (desc.PS.pShaderBytecode != nullptr)
			key = d3d12::HashBytes(std::span(static_cast<const std::byte*>(desc.PS.pShaderBytecode), desc.PS.BytecodeLength), key);
		key = HashBytesOf(layout->rootSignatureHash, key);
		key = HashBytesOf(desc.BlendState.AlphaToCoverageEnable, key);
		key = HashBytesOf(desc.BlendState.IndependentBlendEnable, key);
		for (const auto& target : desc.BlendState.RenderTarget)
		{
			for (auto value : {target.BlendEnable, target.LogicOpEnable})
				key = HashBytesOf(value, key);
			for (auto value : {target.SrcBlend, target.DestBlend, target.SrcBlendAlpha, target.DestBlendAlpha})
				key = HashBytesOf(value, key);
			key = HashBytesOf(target.BlendOp, HashBytesOf(target.BlendOpAlpha, HashBytesOf(target.LogicOp, key)));
			key = HashBytesOf(target.RenderTargetWriteMask, key);
		}
		key = HashBytesOf(desc.SampleMask, key);
		key = HashBytesOf(desc.RasterizerState, key); // (4 byte fields alone: no padding)
		const auto& depth = desc.DepthStencilState;
		key = HashBytesOf(depth.DepthEnable, HashBytesOf(depth.DepthWriteMask, HashBytesOf(depth.DepthFunc, key)));
		key = HashBytesOf(depth.StencilEnable, HashBytesOf(depth.StencilReadMask, HashBytesOf(depth.StencilWriteMask, key)));
		key = HashBytesOf(depth.FrontFace, HashBytesOf(depth.BackFace, key)); // (4 byte fields alone)
		key = HashBytesOf(desc.IBStripCutValue, HashBytesOf(desc.PrimitiveTopologyType, HashBytesOf(desc.NumRenderTargets, key)));
		key = HashBytesOf(desc.RTVFormats, HashBytesOf(desc.DSVFormat, key));
		key = HashBytesOf(desc.SampleDesc, HashBytesOf(desc.Flags, key));
		object->key = key;
	}
	// the state draws take by default (no culling, counter-clockwise front faces) made now, the others at their first
	(void)object->State(GetDesc().device, CullMode::kNone, FrontFace::kCounterClockwise);
	pipeline::LogCreation("graphics", object->name.c_str(), variant.specialization, creationStart);

	return object;
}

template <>
PipelineHandle<kD3D12> Pipeline<kD3D12>::InternalCreateComputePipeline(
	uint64_t /*hashKey*/, ComputePipelineVariant variant, const PipelineShaderStageCreateInfo<kD3D12>& stage, PipelineLayoutHandle<kD3D12> layout)
{
	ZoneScopedN("Pipeline::InternalCreateComputePipeline");

	auto creationStart = std::chrono::steady_clock::now();
	// the variant's specialization: a binary of its own (D3D12 has no specialization constants, see ShaderModuleObject)
	const auto& code = stage.module->Code(variant.specialization);
	const D3D12_COMPUTE_PIPELINE_STATE_DESC desc{
		.pRootSignature = layout->rootSignature,
		.CS = {.pShaderBytecode = code.data(), .BytecodeLength = code.size()},
		.NodeMask = 0,
		.CachedPSO = {},
		.Flags = D3D12_PIPELINE_STATE_FLAG_NONE,
	};
	// its name in the cache: its shader's bytecode and its root signature's
	const uint64_t key = d3d12::HashBytesOf(layout->rootSignatureHash, d3d12::HashBytes(std::as_bytes(std::span(code))));
	ID3D12PipelineState* state = d3d12::PipelineCacheObject::Compute(myCache, GetDesc().device, desc, key, stage.name);
	d3d12::SetName(state, stage.name);
	pipeline::LogCreation("compute", stage.name, variant.specialization, creationStart);

	auto* object = new d3d12::PipelineObject{};
	object->state = state;
	object->rootSignature = layout->rootSignature;
	object->layout = layout;
	object->compute = true;
	object->name = stage.name;
	return object;
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
	myCache = d3d12::pipelinecache::Open(GetDesc().device, d3d12::pipelinecache::PathOf(GetDesc().cachePath));
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
		for (auto*& state : handle->states)
			d3d12::Release(state);
		delete handle;
	}

	myDescriptorMap.clear();
	d3d12::pipelinecache::Close(myCache, d3d12::pipelinecache::PathOf(GetDesc().cachePath));
}

} // namespace rhi
