#include <rhi/pipeline.h>
#include <rhi/rhi.h>
#include <rhi/vulkan/convert.h>
#include <rhi/vulkan/utils.h>

namespace rhi
{

// the pipeline's descriptor sets: their state per layout, updated (or pushed) when their data changed, and bound
template <>
void Pipeline<kVk>::InternalPrepareDescriptorSets()
{
	const auto layoutIt = InternalGetLayout();
	ENSURE(layoutIt != myPipelineLayouts.end());
	const auto& layout = *layoutIt;

	for (const auto& [set, setLayout] : layout.GetDescriptorSetLayouts())
	{
		auto* setLayoutHandle = static_cast<DescriptorSetLayoutHandle<kVk>>(setLayout);
		auto setStateIt = myDescriptorMap.find(setLayoutHandle);
		if (setStateIt == myDescriptorMap.end())
		{
			auto insertResultPair = myDescriptorMap.emplace(
				setLayoutHandle,
				std::make_tuple(
					core::UpgradableSharedMutex{},
					DescriptorSetStatus::kReady,
					BindingsMap<kVk>{},
					BindingsData<kVk>{},
					DescriptorUpdateTemplate<kVk>{
						DescriptorUpdateTemplateCreateDesc<kVk>{
							GetDevice().CreateDeviceObjectCreateDesc("DescriptorUpdateTemplate"),
							((setLayout.GetDesc().flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR) != 0U)
								? VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR
								: VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET,
							static_cast<VkDescriptorSetLayout>(setLayout),
							myBindPoint,
							static_cast<VkPipelineLayout>(layout),
							set,}},
					((setLayout.GetDesc().flags &
					VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR) != 0U)
						? std::nullopt
						: std::make_optional(DescriptorSetArrayList<kVk>{})));
			auto& [insertIt, insertResult] = insertResultPair;
			ENSURE(insertResult);
			ENSURE(insertIt != myDescriptorMap.end());
			auto& [bindingIndex, bindingTuple] = *insertIt;
			auto& [mutex, setState, bindingsMap, bindingsData, setTemplate, setOptionalArrayList] = bindingTuple;

			if (setOptionalArrayList)
			{
				auto& setArrayList = setOptionalArrayList.value();

				setArrayList.emplace_front(
					DescriptorSetArray<kVk>(
						DescriptorSetArrayCreateDesc<kVk>{
							GetDevice().CreateDeviceObjectCreateDesc("DescriptorSetArray"),
							myDescriptorPool,
						},
						setLayout),
					0);
			}
		}
	}
}

template <>
void Pipeline<kVk>::InternalResetDescriptorPool()
{
	vkResetDescriptorPool(GetDevice(), myDescriptorPool, 0);
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
				DescriptorSetArrayCreateDesc<kVk>{GetDevice().CreateDeviceObjectCreateDesc("DescriptorSetArray"), myDescriptorPool}, setLayout),
			~0));
	}

	auto& [setArray, setIndex] = setArrayList.front();
	++setIndex;
	ENSURE(setIndex < setArray.Capacity());
	auto* setHandle = setArray[setIndex];

	{
		ZoneScopedN(
			"Pipeline::InternalUpdateDescriptorSet::vkUpdateDescriptorSetWithTemplate");

		vkUpdateDescriptorSetWithTemplate(
			GetDevice(), setHandle, setTemplate, bindingsData.data());
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

	vkCmdPushDescriptorSetWithTemplate(
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
				.dstBinding=index,
				.dstArrayElement=low,
				.descriptorCount=rangeCount,
				.descriptorType=type,
				.offset=(offset + rangeOffset) * sizeof(BindingVariant<kVk>),
				.stride=sizeof(BindingVariant<kVk>)});

			rangeOffset += rangeCount;
		}
	}

	setTemplate.SetEntries(std::move(entries));
}

template <>
void Pipeline<kVk>::BindDescriptorSet(
	CommandBufferHandle<kVk> cmd,
	DescriptorSetHandle<kVk> handle,
	PipelineBindPoint bindPoint,
	PipelineLayoutHandle<kVk> layoutHandle,
	uint32_t set,
	std::optional<uint32_t> bufferOffset) const
{
	ZoneScopedN("Pipeline::BindDescriptorSet");

	vkCmdBindDescriptorSets(
		cmd,
		vk::ToVk(bindPoint),
		layoutHandle,
		set,
		1,
		&handle,
		bufferOffset ? 1 : 0,
		bufferOffset ? &bufferOffset.value() : nullptr);
}

template <>
void Pipeline<kVk>::InvalidateDescriptorSets()
{
	for (auto& [setLayout, state] : myDescriptorMap)
	{
		auto& [mutex, setState, bindingsMap, bindingsData, setTemplate, setOptionalArrayList] = state;
		auto lock = std::lock_guard(mutex);
		std::atomic_ref(setState).store(DescriptorSetStatus::kDirty, std::memory_order_release);
	}
}

template <>
std::shared_ptr<void> Pipeline<kVk>::TakeDescriptorSets(uint32_t set)
{
	const auto layoutIt = InternalGetLayout();
	ENSURE(layoutIt != myPipelineLayouts.end());
	const auto& setLayout = layoutIt->GetDescriptorSetLayout(set);
	auto& [mutex, setState, bindingsMap, bindingsData, setTemplate, setOptionalArrayList] = myDescriptorMap.at(setLayout);
	auto lock = std::lock_guard(mutex);
	if (!setOptionalArrayList || setOptionalArrayList->empty())
		return {};
	auto taken = std::make_shared<DescriptorSetArrayList<kVk>>(std::exchange(*setOptionalArrayList, {}));
	std::atomic_ref(setState).store(DescriptorSetStatus::kDirty, std::memory_order_release);
	return taken;
}

template <>
void Pipeline<kVk>::BindDescriptorSetAuto(
	CommandBufferHandle<kVk> cmd,
	uint32_t set,
	std::optional<uint32_t> bufferOffset)
{
	ZoneScopedN("Pipeline::BindDescriptorSetAuto");

	const auto layoutIt = InternalGetLayout();
	ENSURE(layoutIt != myPipelineLayouts.end());
	const auto& layout = *layoutIt;
	const auto& setLayout = layout.GetDescriptorSetLayout(set);
	auto& [mutex, setState, bindingsMap, bindingsData, setTemplate, setOptionalArrayList] = myDescriptorMap.at(setLayout);
	
	if (setOptionalArrayList)
	{
		mutex.lock_upgrade();

		auto dirtyState = DescriptorSetStatus::kDirty;
		if (std::atomic_ref(setState).compare_exchange_weak(dirtyState, DescriptorSetStatus::kReady, std::memory_order_acq_rel))
		{
			mutex.unlock_upgrade_and_lock();

			InternalUpdateDescriptorSet(
				setLayout, bindingsData, setTemplate, setOptionalArrayList.value());

			mutex.unlock_and_lock_shared();
		}
		else [[likely]]
		{
			mutex.unlock_upgrade_and_lock_shared();
		}

		auto& setArrayList = setOptionalArrayList.value();
		ENSURE(!setArrayList.empty());
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
	else // push descriptors: core in vulkan 1.4
	{
		mutex.lock_shared();

		InternalPushDescriptorSet(
			cmd,
			static_cast<PipelineLayoutHandle<kVk>>(layout),
			bindingsData,
			setTemplate);
	}

	mutex.unlock_shared();
}

} // namespace rhi
