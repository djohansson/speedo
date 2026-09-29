#include <rhi/descriptorset.h>
#include <rhi/device.h>
#include <rhi/instance.h>
#include <rhi/rhiapplication.h>
#include <rhi/vulkan/utils.h>

#include <utility>

namespace rhi
{

IMPLEMENT_OBJECT_GETINSTANCE(DescriptorSetLayout<kVk>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(DescriptorSetLayout<kVk>);
IMPLEMENT_OBJECT_GETINSTANCE(DescriptorSetArray<kVk>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(DescriptorSetArray<kVk>);
IMPLEMENT_OBJECT_GETINSTANCE(DescriptorUpdateTemplate<kVk>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(DescriptorUpdateTemplate<kVk>);

template <>
void DescriptorSetLayout<kVk>::Swap(DescriptorSetLayout& rhs) noexcept
{
	DeviceObject<DescriptorSetLayout<kVk>>::Swap(rhs);
	std::swap(myLayout, rhs.myLayout);
}

template <>
DescriptorSetLayout<kVk>::DescriptorSetLayout(DescriptorSetLayout&& other) noexcept
{
	Swap(other);
}

template <>
DescriptorSetLayout<kVk>::DescriptorSetLayout(
	CreateDescType&& desc,
	ValueType&& layout)
	: DeviceObject<DescriptorSetLayout<kVk>>(std::forward<CreateDescType>(desc))
	, myLayout(std::forward<ValueType>(layout))
{}

template <>
DescriptorSetLayout<kVk>::DescriptorSetLayout(CreateDescType&& desc)
	: DescriptorSetLayout(
		std::forward<CreateDescType>(desc),
		// read from desc, not GetDesc(): this runs before the delegated constructor has initialized the base
		[this, &desc]
		{
			auto samplers = SamplerVector<kVk>(
				SamplerVectorCreateDesc<kVk>{
					SuperType::CreateDeviceObjectCreateDesc("ImmutableSamplers", desc.device),
					desc.immutableSamplers});

			ShaderVariableBindingsMap bindingsMap;
			auto& bindings = desc.bindings;
			for (size_t bindingIt = 0; bindingIt < bindings.size(); bindingIt++)
			{
				auto& binding = bindings[bindingIt];
				binding.pImmutableSamplers = samplers.Data();
				bindingsMap.emplace(
					desc.variableNameHashes.at(bindingIt),
					std::make_tuple(
						binding.binding, binding.descriptorType, binding.descriptorCount));
			}

			const auto& bindingFlags = desc.bindingFlags;

			ENSURE(bindings.size() == bindingFlags.size());

			VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsInfo{
				.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
			bindingFlagsInfo.bindingCount = bindings.size();
			bindingFlagsInfo.pBindingFlags = bindingFlags.data();

			VkDescriptorSetLayoutCreateInfo layoutInfo{
				.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
			layoutInfo.pNext = &bindingFlagsInfo;
			layoutInfo.flags = desc.flags;
			layoutInfo.bindingCount = bindings.size();
			layoutInfo.pBindings = bindings.data();

			VkDescriptorSetLayout layout;
			VK_CHECK(vkCreateDescriptorSetLayout(
				desc.device,
				&layoutInfo,
				&GetInstance().GetHostAllocationCallbacks(),
				&layout));

			return std::make_tuple(layout, std::move(samplers), std::move(bindingsMap));
		}())
{}

template <>
DescriptorSetLayout<kVk>::~DescriptorSetLayout()
{
	if (IsValid())
	{
		ZoneScopedN("DescriptorSetLayout::vkDestroyDescriptorSetLayout");

		vkDestroyDescriptorSetLayout(
			GetDevice(),
			std::get<0>(myLayout),
			&GetInstance().GetHostAllocationCallbacks());
	}
}

template <>
DescriptorSetLayout<kVk>& DescriptorSetLayout<kVk>::operator=(DescriptorSetLayout&& other) noexcept
{
	Swap(other);
	return *this;
}

template <>
DescriptorSetArray<kVk>::DescriptorSetArray(
	CreateDescType&& desc,
	ArrayType&& descriptorSetHandles)
	: DeviceObject<DescriptorSetArray<kVk>>(std::forward<CreateDescType>(desc))
	, myDescriptorSets(std::forward<ArrayType>(descriptorSetHandles))
{}

template <>
void DescriptorSetArray<kVk>::Swap(DescriptorSetArray& rhs) noexcept
{
	DeviceObject<DescriptorSetArray<kVk>>::Swap(rhs);
	std::swap(myDescriptorSets, rhs.myDescriptorSets);
}

template <>
DescriptorSetArray<kVk>::DescriptorSetArray(DescriptorSetArray&& other) noexcept
{
	Swap(other);
}

template <>
DescriptorSetArray<kVk>::DescriptorSetArray(
	CreateDescType&& desc,
	const DescriptorSetLayout<kVk>& layout)
	: DescriptorSetArray(
		std::forward<CreateDescType>(desc),
		// read from desc, not GetDesc(): this runs before the delegated constructor has initialized the base
		[&layout, &desc]
		{
			std::array<VkDescriptorSetLayout, kDescriptorSetCount> layouts;
			layouts.fill(layout);

			ArrayType sets;
			VkDescriptorSetAllocateInfo allocInfo{
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
			allocInfo.descriptorPool = desc.pool;
			allocInfo.descriptorSetCount = layouts.size();
			allocInfo.pSetLayouts = layouts.data();
			VK_CHECK(vkAllocateDescriptorSets(desc.device, &allocInfo, sets.data()));

			return sets;
		}())
{}

template <>
DescriptorSetArray<kVk>::~DescriptorSetArray()
{
	if (IsValid())
		vkFreeDescriptorSets(
			GetDevice(),
			GetDesc().pool,
			myDescriptorSets.size(),
			myDescriptorSets.data());
}

template <>
DescriptorSetArray<kVk>& DescriptorSetArray<kVk>::operator=(DescriptorSetArray&& other) noexcept
{
	Swap(other);
	return *this;
}

template <>
void DescriptorUpdateTemplate<kVk>::InternalDestroyTemplate()
{
	ZoneScopedN("DescriptorSetLayout::vkDestroyDescriptorUpdateTemplate");

	vkDestroyDescriptorUpdateTemplate(
		GetDevice(),
		myHandle,
		&GetInstance().GetHostAllocationCallbacks());
}

template <>
void DescriptorUpdateTemplate<kVk>::SetEntries(
	std::vector<DescriptorUpdateTemplateEntry<kVk>>&& entries)
{
	InternalDestroyTemplate();
	myEntries = std::exchange(entries, {});
	myHandle = [this]
	{
		VkDescriptorUpdateTemplate descriptorTemplate;
		VkDescriptorUpdateTemplateCreateInfo createInfo{
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO,
			.pNext = nullptr,
			.flags = 0, // reserved for future use
			.descriptorUpdateEntryCount = static_cast<uint32_t>(myEntries.size()),
			.pDescriptorUpdateEntries = myEntries.data(),
			.templateType = GetDesc().templateType,
			.descriptorSetLayout = GetDesc().descriptorSetLayout,
			.pipelineBindPoint = GetDesc().pipelineBindPoint,
			.pipelineLayout = GetDesc().pipelineLayout,
			.set = GetDesc().set};
		vkCreateDescriptorUpdateTemplate(
			GetDevice(),
			&createInfo,
			&GetInstance().GetHostAllocationCallbacks(),
			&descriptorTemplate);

		return descriptorTemplate;
	}();
}

template <>
void DescriptorUpdateTemplate<kVk>::Swap(DescriptorUpdateTemplate& rhs) noexcept
{
	DeviceObject<DescriptorUpdateTemplate<kVk>>::Swap(rhs);
	std::swap(myEntries, rhs.myEntries);
	std::swap(myHandle, rhs.myHandle);
}

template <>
DescriptorUpdateTemplate<kVk>::DescriptorUpdateTemplate(DescriptorUpdateTemplate&& other) noexcept
{
	Swap(other);
}

template <>
DescriptorUpdateTemplate<kVk>::DescriptorUpdateTemplate(
	CreateDescType&& desc,
	DescriptorUpdateTemplateHandle<kVk>&& handle)
	: DeviceObject<DescriptorUpdateTemplate<kVk>>(std::forward<CreateDescType>(desc))
	, myHandle(std::forward<DescriptorUpdateTemplateHandle<kVk>>(handle))
{}

template <>
DescriptorUpdateTemplate<kVk>::DescriptorUpdateTemplate(
	DescriptorUpdateTemplateCreateDesc<kVk>&& desc)
	: DescriptorUpdateTemplate(
		std::forward<DescriptorUpdateTemplateCreateDesc<kVk>>(desc),
		VK_NULL_HANDLE)
{}

template <>
DescriptorUpdateTemplate<kVk>::~DescriptorUpdateTemplate()
{
	if (IsValid())
		InternalDestroyTemplate();
}

template <>
DescriptorUpdateTemplate<kVk>&
DescriptorUpdateTemplate<kVk>::operator=(DescriptorUpdateTemplate&& other) noexcept
{
	Swap(other);
	return *this;
}

} // namespace rhi
