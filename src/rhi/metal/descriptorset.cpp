// the Metal backend's descriptor sets: a layout is an argument buffer's (tier 2: 8 byte entries, a binding's at 8 times
// the counts of the bindings before it, see backend.h), an array of sets one buffer in shared storage holding each set's
// argument buffer, and an update template the entries Pipeline::InternalUpdateDescriptorSet writes them by (no Metal
// object: see docs/backends-plan.md)
#include <rhi/metal/backend.h>

#include <rhi/descriptorset.h>
#include <rhi/device.h>
#include <rhi/rhi.h>

#include <core/assert.h>
#include <core/profiling.h>

#include <algorithm>
#include <utility>

namespace rhi
{

namespace metal
{

const DescriptorSetLayoutObject::Binding& DescriptorSetLayoutObject::Find(uint32_t binding) const
{
	auto bindingIt = std::ranges::lower_bound(bindings, binding, {}, &Binding::binding);
	ENSUREF(bindingIt != bindings.end() && bindingIt->binding == binding, "the descriptor set layout has no binding {}", binding);
	return *bindingIt;
}

} // namespace metal

IMPLEMENT_OBJECT_GETINSTANCE(DescriptorSetLayout<kMetal>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(DescriptorSetLayout<kMetal>);
IMPLEMENT_OBJECT_GETINSTANCE(DescriptorSetArray<kMetal>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(DescriptorSetArray<kMetal>);
IMPLEMENT_OBJECT_GETINSTANCE(DescriptorUpdateTemplate<kMetal>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(DescriptorUpdateTemplate<kMetal>);

namespace descriptorset
{

// a set's part of an array's buffer: its argument buffer's size rounded up to what a constant buffer's offset takes on
// every Mac (256 bytes: Apple's gpus take 32, AMD's and Intel's 256)
static constexpr uint64_t kSetAlignment = 256;

} // namespace descriptorset

template <>
DescriptorSetLayout<kMetal>::DescriptorSetLayout(CreateDescType&& desc, ValueType&& layout)
	: DeviceObject<DescriptorSetLayout<kMetal>>(std::forward<CreateDescType>(desc))
	, myLayout(std::forward<ValueType>(layout))
{}

template <>
DescriptorSetLayout<kMetal>::DescriptorSetLayout(CreateDescType&& desc)
	: DescriptorSetLayout(
		std::forward<CreateDescType>(desc),
		// read from desc, not GetDesc(): this runs before the delegated constructor has initialized the base
		[this, &desc]
		{
			auto samplers = SamplerVector<kMetal>(
				SamplerVectorCreateDesc<kMetal>{
					SuperType::CreateDeviceObjectCreateDesc("ImmutableSamplers", desc.device),
					desc.immutableSamplers});

			auto* layout = new metal::DescriptorSetLayoutObject{};
			layout->push = (desc.flags & metal::kDescriptorSetLayoutPush) != 0U;
			ShaderVariableBindingsMap bindingsMap;
			for (size_t bindingIt = 0; bindingIt < desc.bindings.size(); bindingIt++)
			{
				const auto& binding = desc.bindings[bindingIt];
				layout->bindings.push_back({.binding = binding.binding, .type = binding.descriptorType, .count = binding.descriptorCount});
				bindingsMap.emplace(
					desc.variableNameHashes.at(bindingIt),
					std::make_tuple(binding.binding, binding.descriptorType, binding.descriptorCount));
			}
			std::ranges::sort(layout->bindings, {}, &metal::DescriptorSetLayoutObject::Binding::binding);
			for (auto& binding : layout->bindings)
			{
				ENSUREF(
					binding.type != DescriptorType::kInlineUniformBlock,
					"the Metal backend's argument buffers hold no inline uniform blocks yet (binding {})",
					binding.binding);
				binding.offset = layout->size;
				layout->size += metal::DescriptorSetLayoutObject::kEntrySize * binding.count;
			}

			return std::make_tuple(layout, std::move(samplers), std::move(bindingsMap));
		}())
{}

template <>
DescriptorSetLayout<kMetal>::~DescriptorSetLayout()
{
	delete std::get<0>(myLayout);
}

template <>
DescriptorSetArray<kMetal>::DescriptorSetArray(CreateDescType&& desc, ArrayType&& descriptorSetHandles)
	: DeviceObject<DescriptorSetArray<kMetal>>(std::forward<CreateDescType>(desc))
	, myDescriptorSets(std::forward<ArrayType>(descriptorSetHandles))
{}

template <>
DescriptorSetArray<kMetal>::DescriptorSetArray(CreateDescType&& desc, const DescriptorSetLayout<kMetal>& layout)
	: DescriptorSetArray(
		std::forward<CreateDescType>(desc),
		// read from desc, not GetDesc(): this runs before the delegated constructor has initialized the base
		[&layout, &desc]
		{
			ZoneScopedN("DescriptorSetArray::create");

			const auto* layoutObject = static_cast<DescriptorSetLayoutHandle<kMetal>>(layout);
			const uint64_t setSize =
				std::max<uint64_t>(descriptorset::kSetAlignment, (layoutObject->size + descriptorset::kSetAlignment - 1) & ~(descriptorset::kSetAlignment - 1));
			MTL::Buffer* buffer = desc.device->newBuffer(setSize * kDescriptorSetCount, MTL::ResourceStorageModeShared);
			ENSUREF(buffer != nullptr, "Failed to create a Metal argument buffer of {} bytes", setSize * kDescriptorSetCount);
			buffer->setLabel(NS::String::string(desc.name.c_str(), NS::UTF8StringEncoding));

			ArrayType sets;
			for (size_t setIt = 0; setIt < sets.size(); setIt++)
				sets[setIt] = new metal::DescriptorSetObject{.argumentBuffer = buffer->retain(), .offset = setIt * setSize, .resources = {}};
			buffer->release();

			return sets;
		}())
{}

template <>
DescriptorSetArray<kMetal>::~DescriptorSetArray()
{
	if (!IsValid())
		return;

	for (auto* set : myDescriptorSets)
	{
		set->argumentBuffer->release();
		delete set;
	}
}

template <>
void DescriptorUpdateTemplate<kMetal>::InternalDestroyTemplate()
{
	// no Metal object: the entries are the template
}

template <>
void DescriptorUpdateTemplate<kMetal>::SetEntries(std::vector<DescriptorUpdateTemplateEntry<kMetal>>&& entries)
{
	myEntries = std::exchange(entries, {});
}

template <>
DescriptorUpdateTemplate<kMetal>::DescriptorUpdateTemplate(CreateDescType&& desc, DescriptorUpdateTemplateHandle<kMetal>&& handle)
	: DeviceObject<DescriptorUpdateTemplate<kMetal>>(std::forward<CreateDescType>(desc))
	, myHandle(std::forward<DescriptorUpdateTemplateHandle<kMetal>>(handle))
{}

template <>
DescriptorUpdateTemplate<kMetal>::DescriptorUpdateTemplate(DescriptorUpdateTemplateCreateDesc<kMetal>&& desc)
	: DescriptorUpdateTemplate(std::forward<DescriptorUpdateTemplateCreateDesc<kMetal>>(desc), nullptr)
{}

} // namespace rhi
