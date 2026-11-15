// the D3D12 backend's pipeline layouts: a root signature (version 1.1) of the push constants (root constants at their
// register and space), the global uniforms' implicit constant buffer (the specialization constant: a root constant, set
// from the bound pipeline's variant, see PipelineObject), and each set's descriptor tables, the set's CBV, SRV and UAV
// bindings' and its samplers' (backend.h's DescriptorSetLayoutObject: the same ranges, at the same offsets). every range
// volatile: the sets are partly bound (the 1024 textures), and written before the commands that read them run. at most 32
// values of root constants and 2 tables a set: 49 of the root signature's 64 values with 8 sets
#include <rhi/d3d12/backend.h>

#include <rhi/pipeline.h>
#include <rhi/rhi.h>

#include <core/assert.h>
#include <core/uuids_extra.h>

#include <algorithm>
#include <format>
#include <utility>
#include <vector>

namespace rhi
{

IMPLEMENT_OBJECT_GETINSTANCE(PipelineLayout<kD3D12>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(PipelineLayout<kD3D12>);

template <>
PipelineLayout<kD3D12>::PipelineLayout(
	CreateDescType&& desc,
	std::vector<ShaderModule<kD3D12>>&& shaderModules,
	DescriptorSetLayoutFlatMap<kD3D12>&& descriptorSetLayouts,
	PipelineLayoutHandle<kD3D12>&& layout)
	: DeviceObject<PipelineLayout<kD3D12>>(std::forward<CreateDescType>(desc))
	, myShaderModules(std::exchange(shaderModules, {}))
	, myDescriptorSetLayouts(std::exchange(descriptorSetLayouts, {}))
	, myPushConstantRanges(descriptorset::GetPushConstantRanges<kD3D12>(myDescriptorSetLayouts))
	, myLayout(std::forward<PipelineLayoutHandle<kD3D12>>(layout))
{}

// (no constructor from modules and set layouts alone: the root signature needs the shader set's registers)

namespace pipeline
{

// whether a binding is in its set's tables: the push constants' block is a binding of its set too (the reflection's, as
// Vulkan's layouts have it), which DXIL's code has as root constants, no register (the shader compiler fails a parameter
// block's field without one, so a binding without a register is the push constants')
static bool InTables(const ShaderBinding& binding)
{
	return binding.slot != kNoShaderSlot;
}

// a binding's range type in its table
static D3D12_DESCRIPTOR_RANGE_TYPE RangeType(const ShaderBinding& binding)
{
	switch (binding.type)
	{
	case DescriptorType::kSampler: return D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
	case DescriptorType::kUniformBuffer: return D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
	case DescriptorType::kSampledImage:
	case DescriptorType::kAccelerationStructure: return D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	case DescriptorType::kStorageImage: return D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
	case DescriptorType::kStorageBuffer: return binding.writable ? D3D12_DESCRIPTOR_RANGE_TYPE_UAV : D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	default: break;
	}
	ENSUREF(false, "{}: a binding of type {} the D3D12 backend's tables don't hold yet", binding.name, std::to_underlying(binding.type));
	return D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
}

// the layout's handle: its root signature, from the shader set's registers (DXIL's, see rhi::kNoShaderSlot)
static d3d12::PipelineLayoutObject* CreateLayoutObject(ID3D12Device* device, const ShaderSet& shaderSet, std::string_view name)
{
	auto* layout = new d3d12::PipelineLayoutObject{};
	std::vector<D3D12_ROOT_PARAMETER1> parameters;
	std::vector<std::vector<D3D12_DESCRIPTOR_RANGE1>> ranges; // each table's, kept until serialized
	ranges.reserve(shaderSet.layouts.size() * 2);

	// the sets in their order, each set's bindings in theirs (as DescriptorSetLayout lays out its tables)
	std::vector<uint32_t> setIndices;
	for (const auto& [set, setLayout] : shaderSet.layouts)
		setIndices.push_back(set);
	std::ranges::sort(setIndices);
	for (uint32_t set : setIndices)
	{
		const auto& setLayout = shaderSet.layouts.at(set);
		if (const auto& pushConstants = setLayout.pushConstants)
		{
			ENSUREF(
				pushConstants->slot != kNoShaderSlot, "{}: the push constants have no register (the shader set's code isn't DXIL?)", name);
			ENSUREF(layout->pushConstants == d3d12::PipelineLayoutObject::kNone, "{}: push constants in two sets", name);
			layout->pushConstants = static_cast<uint32_t>(parameters.size());
			layout->pushConstantCount = (pushConstants->offset + pushConstants->size + 3) / 4;
			parameters.push_back(D3D12_ROOT_PARAMETER1{
				.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
				.Constants = {.ShaderRegister = pushConstants->slot, .RegisterSpace = pushConstants->space, .Num32BitValues = layout->pushConstantCount},
				.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL});
		}
		std::vector<const ShaderBinding*> bindings;
		for (const auto& binding : setLayout.bindings)
			if (pipeline::InTables(binding))
				bindings.push_back(&binding);
		if (bindings.empty())
			continue;
		ENSUREF(setLayout.slot != kNoShaderSlot, "{}: set {} has no register space (the shader set's code isn't DXIL?)", name, set);
		std::ranges::sort(bindings, {}, &ShaderBinding::binding);
		std::vector<D3D12_DESCRIPTOR_RANGE1> resources;
		std::vector<D3D12_DESCRIPTOR_RANGE1> samplers;
		for (const auto* binding : bindings)
		{
			ENSUREF(binding->slot != kNoShaderSlot, "{}: {} has no register", name, binding->name);
			const auto type = RangeType(*binding);
			auto& table = type == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER ? samplers : resources;
			const UINT offset = table.empty() ? 0 : table.back().OffsetInDescriptorsFromTableStart + table.back().NumDescriptors;
			table.push_back(D3D12_DESCRIPTOR_RANGE1{
				.RangeType = type,
				.NumDescriptors = binding->count,
				.BaseShaderRegister = binding->slot,
				.RegisterSpace = setLayout.slot,
				.Flags = type == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER
							 ? D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE
							 : D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE,
				.OffsetInDescriptorsFromTableStart = offset});
		}
		if (layout->sets.size() <= set)
			layout->sets.resize(set + 1);
		auto table = [&](std::vector<D3D12_DESCRIPTOR_RANGE1>&& tableRanges) -> uint32_t
		{
			if (tableRanges.empty())
				return d3d12::PipelineLayoutObject::kNone;
			auto& kept = ranges.emplace_back(std::move(tableRanges));
			parameters.push_back(D3D12_ROOT_PARAMETER1{
				.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
				.DescriptorTable = {.NumDescriptorRanges = static_cast<UINT>(kept.size()), .pDescriptorRanges = kept.data()},
				.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL});
			return static_cast<uint32_t>(parameters.size() - 1);
		};
		layout->sets[set].resources = table(std::move(resources));
		layout->sets[set].samplers = table(std::move(samplers));
	}
	// the specialization constant (slang's [vk::constant_id] uniform, in space 0's implicit constant buffer)
	if (shaderSet.globalsSize > 0)
	{
		layout->globals = static_cast<uint32_t>(parameters.size());
		parameters.push_back(D3D12_ROOT_PARAMETER1{
			.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
			.Constants = {.ShaderRegister = shaderSet.globalsSlot, .RegisterSpace = 0, .Num32BitValues = (shaderSet.globalsSize + 3) / 4},
			.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL});
	}

	const D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{
		.Version = D3D_ROOT_SIGNATURE_VERSION_1_1,
		.Desc_1_1 = {
			.NumParameters = static_cast<UINT>(parameters.size()),
			.pParameters = parameters.data(),
			.NumStaticSamplers = 0,
			.pStaticSamplers = nullptr,
			.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE}};
	ID3DBlob* blob = nullptr;
	ID3DBlob* error = nullptr;
	const HRESULT result = D3D12SerializeVersionedRootSignature(&desc, &blob, &error);
	ENSUREF(
		SUCCEEDED(result),
		"{}: D3D12SerializeVersionedRootSignature failed ({:#010x}): {}",
		name,
		static_cast<uint32_t>(result),
		error != nullptr ? std::string_view(static_cast<const char*>(error->GetBufferPointer()), error->GetBufferSize()) : "");
	d3d12::Release(error);
	d3d12::Check(
		device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&layout->rootSignature)),
		std::format("{}: CreateRootSignature", name));
	d3d12::Release(blob);
	d3d12::SetName(layout->rootSignature, name);
	return layout;
}

// a shader set layout as the backend takes it: each binding's register, its set's space, a storage buffer's view; a
// pushed set's (see Pipeline::CreateLayout) flagged
static DescriptorSetLayoutCreateDesc<kD3D12> ToDescriptorSetLayoutCreateDesc(const ShaderSetLayout& layout, bool push)
{
	DescriptorSetLayoutCreateDesc<kD3D12> desc;
	if (push)
		desc.flags |= d3d12::kDescriptorSetLayoutPush;
	for (const auto& binding : layout.bindings)
	{
		if (!InTables(binding))
			continue;
		desc.bindings.push_back({
			.binding = binding.binding,
			.descriptorType = binding.type,
			.descriptorCount = binding.count,
			.shaderRegister = binding.slot,
			.registerSpace = layout.slot,
			.stride = binding.stride,
			.writable = binding.writable,
		});
		desc.bindingFlags.push_back(0);
		desc.variableNames.push_back(binding.name);
		desc.variableNameHashes.push_back(binding.nameHash);
	}
	if (const auto& pushConstants = layout.pushConstants)
		desc.pushConstantRange =
			PushConstantRange<kD3D12>{.stages = pushConstants->stages, .offset = pushConstants->offset, .size = pushConstants->size};
	return desc;
}

} // namespace pipeline

template <>
PipelineLayout<kD3D12>::PipelineLayout(CreateDescType&& desc, const ShaderSet& shaderSet, std::span<const uint32_t> pushDescriptorSets)
	: PipelineLayout(
		std::forward<CreateDescType>(desc),
		// read from desc, not GetDesc(): this runs before the delegated constructor has initialized the base
		[&shaderSet, &desc, this]
		{
			std::vector<ShaderModule<kD3D12>> shaderModules;
			shaderModules.reserve(shaderSet.shaders.size());
			for (const auto& shader : shaderSet.shaders)
				shaderModules.emplace_back(
					ShaderModuleCreateDesc<kD3D12>{
						SuperType::CreateDeviceObjectCreateDesc("ShaderModule", desc.device),
						shader,
					});
			return shaderModules;
		}(),
		[&shaderSet, &desc, pushDescriptorSets]
		{
			DescriptorSetLayoutFlatMap<kD3D12> map;
			for (const auto& [set, shaderSetLayout] : shaderSet.layouts)
			{
				bool push = std::ranges::find(pushDescriptorSets, set) != pushDescriptorSets.end();
				auto layout = pipeline::ToDescriptorSetLayoutCreateDesc(shaderSetLayout, push);
				layout.instance = desc.instance;
				layout.device = desc.device;
				layout.uuid = uuids::NewUuid();
				layout.name = std::format("{} DescriptorSetLayout {}", GetDebugName(desc), set);
				map.emplace(set, DescriptorSetLayout<kD3D12>(std::move(layout)));
			}
			return map;
		}(),
		pipeline::CreateLayoutObject(desc.device, shaderSet, desc.name))
{}

template <>
PipelineLayout<kD3D12>::~PipelineLayout()
{
	if (myLayout == nullptr)
		return;
	d3d12::Release(myLayout->rootSignature);
	delete myLayout;
}

} // namespace rhi
