// the Metal backend's descriptor sets in the pipeline: their state per layout, written into a fresh set's argument buffer
// when their values changed (or, for a pushed set, into the command buffer's own at each bind, set by setBytes), and
// bound for the next dispatch with what they reference (see commandencoder.cpp)
#include <rhi/metal/backend.h>

#include <rhi/pipeline.h>
#include <rhi/rhi.h>

#include <core/assert.h>
#include <core/profiling.h>

#include <cstring>
#include <variant>

namespace rhi
{

namespace pipeline
{

// a set's values written into its argument buffer, by the template's entries (see DescriptorSetLayoutObject), and what
// they reference collected (buffers, textures, acceleration structures: samplers aren't resources)
static void EncodeDescriptorSet(
	const metal::DescriptorSetLayoutObject& layout,
	const BindingsData<kMetal>& bindingsData,
	const std::vector<DescriptorUpdateTemplateEntry<kMetal>>& entries,
	uint8_t* argumentBuffer,
	std::vector<MTL::Resource*>& resources)
{
	ZoneScopedN("Pipeline::EncodeDescriptorSet");

	static_assert(sizeof(MTL::ResourceID) == metal::DescriptorSetLayoutObject::kEntrySize);

	resources.clear();
	std::memset(argumentBuffer, 0, layout.size);
	for (const auto& entry : entries)
	{
		const auto& binding = layout.Find(entry.dstBinding);
		ENSUREF(
			entry.dstArrayElement + entry.descriptorCount <= binding.count,
			"binding {}'s elements {} to {} are past its count {}",
			entry.dstBinding,
			entry.dstArrayElement,
			entry.dstArrayElement + entry.descriptorCount,
			binding.count);
		for (uint32_t elementIt = 0; elementIt < entry.descriptorCount; elementIt++)
		{
			const auto& value = bindingsData[(entry.offset + (elementIt * entry.stride)) / sizeof(BindingVariant<kMetal>)];
			uint8_t* slot = argumentBuffer + binding.offset +
							(metal::DescriptorSetLayoutObject::kEntrySize * (entry.dstArrayElement + elementIt));
			if (const auto* buffer = std::get_if<DescriptorBufferInfo<kMetal>>(&value))
			{
				ENSUREF(buffer->buffer != nullptr, "binding {} has no buffer", entry.dstBinding);
				uint64_t address = buffer->buffer->gpuAddress() + buffer->offset;
				std::memcpy(slot, &address, sizeof(address));
				resources.push_back(buffer->buffer);
			}
			else if (const auto* image = std::get_if<DescriptorImageInfo<kMetal>>(&value))
			{
				// a texture (sampled or storage) or a sampler, each an entry of its own: slang splits a combined image
				// sampler into two fields, which a layout's binding of one count doesn't make room for
				ENSUREF(
					entry.descriptorType != DescriptorType::kCombinedImageSampler,
					"the Metal backend's argument buffers hold no combined image samplers (binding {}): bind the texture and sampler apart",
					entry.dstBinding);
				MTL::ResourceID id{};
				if (image->imageView != nullptr)
				{
					id = image->imageView->texture->gpuResourceID();
					resources.push_back(image->imageView->texture);
				}
				else
				{
					ENSUREF(image->sampler != nullptr, "binding {} has neither an image nor a sampler", entry.dstBinding);
					id = image->sampler->gpuResourceID();
				}
				std::memcpy(slot, &id, sizeof(id));
			}
			else if (const auto* accelerationStructure = std::get_if<AccelerationStructureHandle<kMetal>>(&value))
			{
				ENSUREF(*accelerationStructure != nullptr, "binding {} has no acceleration structure", entry.dstBinding);
				MTL::ResourceID id = (*accelerationStructure)->gpuResourceID();
				std::memcpy(slot, &id, sizeof(id));
				resources.push_back(*accelerationStructure);
			}
			else
			{
				ENSUREF(false, "the Metal backend's argument buffers hold no texel buffers or inline uniform blocks yet (binding {})", entry.dstBinding);
			}
		}
	}
}

} // namespace pipeline

template <>
void Pipeline<kMetal>::InternalPrepareDescriptorSets()
{
	const auto layoutIt = InternalGetLayout();
	ENSURE(layoutIt != myPipelineLayouts.end());
	const auto& layout = *layoutIt;

	for (const auto& [set, setLayout] : layout.GetDescriptorSetLayouts())
	{
		auto* setLayoutHandle = static_cast<DescriptorSetLayoutHandle<kMetal>>(setLayout);
		if (myDescriptorMap.contains(setLayoutHandle))
			continue;

		const bool push = (setLayout.GetDesc().flags & metal::kDescriptorSetLayoutPush) != 0U;
		auto [insertIt, insertResult] = myDescriptorMap.emplace(
			setLayoutHandle,
			std::make_tuple(
				core::UpgradableSharedMutex{},
				DescriptorSetStatus::kReady,
				BindingsMap<kMetal>{},
				BindingsData<kMetal>{},
				DescriptorUpdateTemplate<kMetal>{DescriptorUpdateTemplateCreateDesc<kMetal>{
					GetDevice().CreateDeviceObjectCreateDesc("DescriptorUpdateTemplate"),
					push ? metal::DescriptorUpdateTemplateType::kPushDescriptors : metal::DescriptorUpdateTemplateType::kDescriptorSet,
					setLayoutHandle,
					myBindPoint,
					static_cast<PipelineLayoutHandle<kMetal>>(layout),
					set,
				}},
				push ? std::nullopt : std::make_optional(DescriptorSetArrayList<kMetal>{})));
		ENSURE(insertResult);
		auto& [mutex, setState, bindingsMap, bindingsData, setTemplate, setOptionalArrayList] = insertIt->second;

		if (setOptionalArrayList)
			setOptionalArrayList->emplace_front(
				DescriptorSetArray<kMetal>(
					DescriptorSetArrayCreateDesc<kMetal>{
						GetDevice().CreateDeviceObjectCreateDesc("DescriptorSetArray"),
						myDescriptorPool,
						myDescriptorPoolMutex,
					},
					setLayout),
				0);
	}
}

template <>
void Pipeline<kMetal>::InternalResetDescriptorPool()
{
	// no pool: each array of sets is a buffer of its own (see descriptorset.cpp)
}

template <>
void Pipeline<kMetal>::InternalUpdateDescriptorSet(
	const DescriptorSetLayout<kMetal>& setLayout,
	const BindingsData<kMetal>& bindingsData,
	const DescriptorUpdateTemplate<kMetal>& setTemplate,
	DescriptorSetArrayList<kMetal>& setArrayList) const
{
	ZoneScopedN("Pipeline::InternalUpdateDescriptorSet");

	// a fresh set (one the gpu may read is never written again), from a new array when the front one is used up
	bool frontArrayIsFull = !setArrayList.empty() && std::get<1>(setArrayList.front()) == (DescriptorSetArray<kMetal>::Capacity() - 1);
	if (setArrayList.empty() || frontArrayIsFull)
		setArrayList.emplace_front(std::make_tuple(
			DescriptorSetArray<kMetal>(
				DescriptorSetArrayCreateDesc<kMetal>{
					GetDevice().CreateDeviceObjectCreateDesc("DescriptorSetArray"), myDescriptorPool, myDescriptorPoolMutex},
				setLayout),
			~0));

	auto& [setArray, setIndex] = setArrayList.front();
	++setIndex;
	ENSURE(setIndex < setArray.Capacity());
	auto* set = setArray[setIndex];

	pipeline::EncodeDescriptorSet(
		*static_cast<DescriptorSetLayoutHandle<kMetal>>(setLayout),
		bindingsData,
		setTemplate.GetEntries(),
		static_cast<uint8_t*>(set->argumentBuffer->contents()) + set->offset,
		set->resources);
}

template <>
void Pipeline<kMetal>::InternalPushDescriptorSet(
	CommandBufferHandle<kMetal> cmd,
	PipelineLayoutHandle<kMetal> layoutHandle,
	const BindingsData<kMetal>& bindingsData,
	const DescriptorUpdateTemplate<kMetal>& setTemplate)
{
	ZoneScopedN("Pipeline::InternalPushDescriptorSet");

	// the set's argument buffer in the command buffer's own memory: setBytes takes 4 KB at most
	constexpr uint32_t kSetBytesCapacity = 4096;
	const auto& layout = *setTemplate.GetDesc().descriptorSetLayout;
	ENSUREF(layout.size <= kSetBytesCapacity, "a pushed set's argument buffer of {} bytes is past setBytes' {}", layout.size, kSetBytesCapacity);

	const uint32_t slot = layoutHandle->SetSlot(setTemplate.GetDesc().set);
	if (cmd->sets.size() <= slot)
		cmd->sets.resize(slot + 1);
	auto& binding = cmd->sets[slot];
	binding.argumentBuffer = nullptr;
	binding.offset = 0;
	binding.pushed.resize(layout.size);
	pipeline::EncodeDescriptorSet(layout, bindingsData, setTemplate.GetEntries(), binding.pushed.data(), binding.resources);
}

template <>
void Pipeline<kMetal>::InternalUpdateDescriptorSetTemplate(
	const BindingsMap<kMetal>& bindingsMap, DescriptorUpdateTemplate<kMetal>& setTemplate)
{
	ZoneScopedN("Pipeline::InternalUpdateDescriptorSetTemplate");

	std::vector<DescriptorUpdateTemplateEntry<kMetal>> entries;
	entries.reserve(bindingsMap.size());

	for (const auto& [index, binding] : bindingsMap)
	{
		const auto& [offset, count, type, ranges] = binding;

		uint32_t rangeOffset = 0;
		for (const auto& [low, high] : ranges)
		{
			entries.push_back(DescriptorUpdateTemplateEntry<kMetal>{
				.dstBinding = index,
				.dstArrayElement = low,
				.descriptorCount = high - low,
				.descriptorType = type,
				.offset = (offset + rangeOffset) * sizeof(BindingVariant<kMetal>),
				.stride = sizeof(BindingVariant<kMetal>)});
			rangeOffset += high - low;
		}
	}

	setTemplate.SetEntries(std::move(entries));
}

template <>
void Pipeline<kMetal>::BindDescriptorSet(
	CommandBufferHandle<kMetal> cmd,
	DescriptorSetHandle<kMetal> handle,
	PipelineBindPoint /*bindPoint*/,
	PipelineLayoutHandle<kMetal> layoutHandle,
	uint32_t set,
	std::optional<uint32_t> bufferOffset) const
{
	ZoneScopedN("Pipeline::BindDescriptorSet");

	ENSUREF(!bufferOffset, "the Metal backend's sets take no dynamic offsets yet");
	const uint32_t slot = layoutHandle->SetSlot(set);
	if (cmd->sets.size() <= slot)
		cmd->sets.resize(slot + 1);
	auto& binding = cmd->sets[slot];
	binding.argumentBuffer = handle->argumentBuffer;
	binding.offset = handle->offset;
	binding.pushed.clear();
	binding.resources = handle->resources;
}

// (InvalidateDescriptorSets, TakeDescriptorSets and BindDescriptorSetAuto are every backend's, in pipeline.inl)

} // namespace rhi
