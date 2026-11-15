// the D3D12 backend's descriptor sets: a layout is the set's two descriptor tables' ranges (its CBV, SRV and UAV
// bindings' in one, its samplers' in the other: a table holds one heap's descriptors), each binding at the register the
// code has it at in the set's space (the shader compiler's, rhi::ShaderBinding::slot); a set is its tables' ranges in the
// device's shader visible heaps, written once, at the update that takes it (Pipeline::InternalUpdateDescriptorSet), as
// Vulkan's are; an update template is the backend's entries alone (no D3D12 object)
#include <rhi/d3d12/backend.h>

#include <rhi/descriptorset.h>
#include <rhi/device.h>
#include <rhi/rhi.h>

#include <core/assert.h>
#include <core/profiling.h>

#include <algorithm>
#include <utility>

namespace rhi
{

IMPLEMENT_OBJECT_GETINSTANCE(DescriptorSetLayout<kD3D12>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(DescriptorSetLayout<kD3D12>);
IMPLEMENT_OBJECT_GETINSTANCE(DescriptorSetArray<kD3D12>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(DescriptorSetArray<kD3D12>);
IMPLEMENT_OBJECT_GETINSTANCE(DescriptorUpdateTemplate<kD3D12>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(DescriptorUpdateTemplate<kD3D12>);

template <>
DescriptorSetLayout<kD3D12>::DescriptorSetLayout(CreateDescType&& desc, ValueType&& layout)
	: DeviceObject<DescriptorSetLayout<kD3D12>>(std::forward<CreateDescType>(desc))
	, myLayout(std::forward<ValueType>(layout))
{}

template <>
DescriptorSetLayout<kD3D12>::DescriptorSetLayout(CreateDescType&& desc)
	: DescriptorSetLayout(
		std::forward<CreateDescType>(desc),
		// read from desc, not GetDesc(): this runs before the delegated constructor has initialized the base
		[this, &desc]
		{
			auto samplers = SamplerVector<kD3D12>(
				SamplerVectorCreateDesc<kD3D12>{
					SuperType::CreateDeviceObjectCreateDesc("ImmutableSamplers", desc.device),
					desc.immutableSamplers});

			auto* layout = new d3d12::DescriptorSetLayoutObject{};
			layout->push = (desc.flags & d3d12::kDescriptorSetLayoutPush) != 0U;
			ShaderVariableBindingsMap bindingsMap;
			for (size_t bindingIt = 0; bindingIt < desc.bindings.size(); bindingIt++)
			{
				const auto& binding = desc.bindings[bindingIt];
				ENSUREF(
					binding.descriptorType != DescriptorType::kInlineUniformBlock && binding.descriptorType != DescriptorType::kCombinedImageSampler &&
						binding.descriptorType != DescriptorType::kUniformTexelBuffer && binding.descriptorType != DescriptorType::kStorageTexelBuffer,
					"the D3D12 backend's sets hold no inline uniform blocks, combined image samplers or texel buffers yet (binding {})",
					binding.binding);
				layout->bindings.push_back({
					.binding = binding.binding,
					.type = binding.descriptorType,
					.count = binding.descriptorCount,
					.shaderRegister = binding.shaderRegister,
					.stride = binding.stride,
					.writable = binding.writable,
					.sampler = binding.descriptorType == DescriptorType::kSampler,
				});
				layout->space = binding.registerSpace;
				bindingsMap.emplace(
					desc.variableNameHashes.at(bindingIt),
					std::make_tuple(binding.binding, binding.descriptorType, binding.descriptorCount));
			}
			std::ranges::sort(layout->bindings, {}, &d3d12::DescriptorSetLayoutObject::Binding::binding);
			for (auto& binding : layout->bindings)
			{
				auto& count = binding.sampler ? layout->samplerCount : layout->resourceCount;
				binding.offset = count;
				count += binding.count;
			}

			return std::make_tuple(layout, std::move(samplers), std::move(bindingsMap));
		}())
{}

template <>
DescriptorSetLayout<kD3D12>::~DescriptorSetLayout()
{
	delete std::get<0>(myLayout);
}

template <>
DescriptorSetArray<kD3D12>::DescriptorSetArray(CreateDescType&& desc, ArrayType&& descriptorSetHandles)
	: DeviceObject<DescriptorSetArray<kD3D12>>(std::forward<CreateDescType>(desc))
	, myDescriptorSets(std::forward<ArrayType>(descriptorSetHandles))
{}

template <>
DescriptorSetArray<kD3D12>::DescriptorSetArray(CreateDescType&& desc, const DescriptorSetLayout<kD3D12>& layout)
	: DescriptorSetArray(
		std::forward<CreateDescType>(desc),
		// read from desc, not GetDesc(): this runs before the delegated constructor has initialized the base
		[&layout, &desc]
		{
			ZoneScopedN("DescriptorSetArray::create");

			const auto* layoutObject = static_cast<DescriptorSetLayoutHandle<kD3D12>>(layout);
			auto& pool = d3d12::AllocatorObject::ForDevice(desc.device).pool;
			ArrayType sets;
			for (auto& set : sets)
				set = new d3d12::DescriptorSetObject{
					.resources = pool.resources.Allocate(layoutObject->resourceCount),
					.samplers = d3d12::GpuDescriptorHeap::kNone, // its samplers' table, at its first write
					.resourceCount = layoutObject->resourceCount,
					.samplerCount = layoutObject->samplerCount,
				};
			return sets;
		}())
{}

template <>
DescriptorSetArray<kD3D12>::~DescriptorSetArray()
{
	if (!IsValid())
		return;

	auto& pool = d3d12::AllocatorObject::ForDevice(GetDesc().device).pool;
	for (auto* set : myDescriptorSets)
	{
		pool.resources.Free(set->resources, set->resourceCount);
		pool.ReleaseSamplerTable(set->samplers);
		delete set;
	}
}

template <>
void DescriptorUpdateTemplate<kD3D12>::InternalDestroyTemplate()
{
	// no D3D12 object: the entries are the template
}

template <>
void DescriptorUpdateTemplate<kD3D12>::SetEntries(std::vector<DescriptorUpdateTemplateEntry<kD3D12>>&& entries)
{
	myEntries = std::exchange(entries, {});
}

template <>
DescriptorUpdateTemplate<kD3D12>::DescriptorUpdateTemplate(CreateDescType&& desc, DescriptorUpdateTemplateHandle<kD3D12>&& handle)
	: DeviceObject<DescriptorUpdateTemplate<kD3D12>>(std::forward<CreateDescType>(desc))
	, myHandle(std::forward<DescriptorUpdateTemplateHandle<kD3D12>>(handle))
{}

template <>
DescriptorUpdateTemplate<kD3D12>::DescriptorUpdateTemplate(DescriptorUpdateTemplateCreateDesc<kD3D12>&& desc)
	: DescriptorUpdateTemplate(std::forward<DescriptorUpdateTemplateCreateDesc<kD3D12>>(desc), nullptr)
{}

} // namespace rhi
