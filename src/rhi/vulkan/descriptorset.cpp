#include "../descriptorset.h"

#include "utils.h"

#include <utility>

template <>
DescriptorSetLayout<kVk>::DescriptorSetLayout(DescriptorSetLayout&& other) noexcept
	: DeviceObject(std::forward<DescriptorSetLayout>(other))
	, myDesc(std::exchange(other.myDesc, {}))
	, myLayout(std::exchange(other.myLayout, {}))
{}

template <>
DescriptorSetLayout<kVk>::DescriptorSetLayout(
	const std::shared_ptr<Device<kVk>>& device,
	DescriptorSetLayoutCreateDesc<kVk>&& desc,
	ValueType&& layout)
	: DeviceObject(
		  device,
		  {"_DescriptorSetLayout"},
		  1,
		  VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT,
		  reinterpret_cast<uint64_t*>(&std::get<0>(layout)))
	, myDesc(std::forward<DescriptorSetLayoutCreateDesc<kVk>>(desc))
	, myLayout(std::forward<ValueType>(layout))
{}

template <>
DescriptorSetLayout<kVk>::DescriptorSetLayout(
	const std::shared_ptr<Device<kVk>>& device,
	DescriptorSetLayoutCreateDesc<kVk>&& desc)
	: DescriptorSetLayout(
		  device,
		  std::forward<DescriptorSetLayoutCreateDesc<kVk>>(desc),
		  [&device, &desc]
		  {
			  auto samplers = SamplerVector<kVk>(device, desc.immutableSamplers);

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

			  auto& bindingFlags = desc.bindingFlags;

			  ASSERT(bindings.size() == bindingFlags.size());

			  VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsInfo{
				  VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
			  bindingFlagsInfo.bindingCount = bindings.size();
			  bindingFlagsInfo.pBindingFlags = bindingFlags.data();

			  VkDescriptorSetLayoutCreateInfo layoutInfo{
				  VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
			  layoutInfo.pNext = &bindingFlagsInfo;
			  layoutInfo.flags = desc.flags;
			  layoutInfo.bindingCount = bindings.size();
			  layoutInfo.pBindings = bindings.data();

			  VkDescriptorSetLayout layout;
			  VK_CHECK(vkCreateDescriptorSetLayout(
				  *device,
				  &layoutInfo,
				  &device->GetInstance()->GetHostAllocationCallbacks(),
				  &layout));

			  return std::make_tuple(layout, std::move(samplers), std::move(bindingsMap));
		  }())
{}

template <>
DescriptorSetLayout<kVk>::~DescriptorSetLayout()
{
	if (auto* layout = std::get<0>(myLayout); layout)
	{
		ZoneScopedN("DescriptorSetLayout::vkDestroyDescriptorSetLayout");

		vkDestroyDescriptorSetLayout(
			*GetDevice(),
			layout,
			&GetDevice()->GetInstance()->GetHostAllocationCallbacks());
	}
}

template <>
DescriptorSetLayout<kVk>& DescriptorSetLayout<kVk>::operator=(DescriptorSetLayout&& other) noexcept
{
	DeviceObject::operator=(std::forward<DescriptorSetLayout>(other));
	myDesc = std::exchange(other.myDesc, {});
	myLayout = std::exchange(other.myLayout, {});
	return *this;
}

template <>
void DescriptorSetLayout<kVk>::Swap(DescriptorSetLayout& rhs) noexcept
{
	DeviceObject::Swap(rhs);
	std::swap(myDesc, rhs.myDesc);
	std::swap(myLayout, rhs.myLayout);
}

template <>
DescriptorSetArray<kVk>::DescriptorSetArray(
	const std::shared_ptr<Device<kVk>>& device,
	DescriptorSetArrayCreateDesc<kVk>&& desc,
	ArrayType&& descriptorSetHandles)
	: DeviceObject(
		  device,
		  {"_DescriptorSet"},
		  descriptorSetHandles.size(),
		  VK_OBJECT_TYPE_DESCRIPTOR_SET,
		  reinterpret_cast<uint64_t*>(descriptorSetHandles.data()))
	, myDesc(std::forward<DescriptorSetArrayCreateDesc<kVk>>(desc))
	, myDescriptorSets(std::forward<ArrayType>(descriptorSetHandles))
{}

template <>
DescriptorSetArray<kVk>::DescriptorSetArray(DescriptorSetArray&& other) noexcept
	: DeviceObject(std::forward<DescriptorSetArray>(other))
	, myDesc(std::exchange(other.myDesc, {}))
	, myDescriptorSets(std::exchange(other.myDescriptorSets, {}))
{}

template <>
DescriptorSetArray<kVk>::DescriptorSetArray(
	const std::shared_ptr<Device<kVk>>& device,
	const DescriptorSetLayout<kVk>& layout,
	DescriptorSetArrayCreateDesc<kVk>&& desc)
	: DescriptorSetArray(
		  device,
		  std::forward<DescriptorSetArrayCreateDesc<kVk>>(desc),
		  [&device, &layout, &desc]
		  {
			  std::array<VkDescriptorSetLayout, kDescriptorSetCount> layouts;
			  layouts.fill(layout);

			  ArrayType sets;
			  VkDescriptorSetAllocateInfo allocInfo{
				  VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
			  allocInfo.descriptorPool = desc.pool;
			  allocInfo.descriptorSetCount = layouts.size();
			  allocInfo.pSetLayouts = layouts.data();
			  VK_CHECK(vkAllocateDescriptorSets(*device, &allocInfo, sets.data()));

			  return sets;
		  }())
{}

template <>
DescriptorSetArray<kVk>::~DescriptorSetArray()
{
	if (IsValid())
		vkFreeDescriptorSets(*GetDevice(), myDesc.pool, myDescriptorSets.size(), myDescriptorSets.data());
}

template <>
DescriptorSetArray<kVk>& DescriptorSetArray<kVk>::operator=(DescriptorSetArray&& other) noexcept
{
	DeviceObject::operator=(std::forward<DescriptorSetArray>(other));
	myDesc = std::exchange(other.myDesc, {});
	myDescriptorSets = std::exchange(other.myDescriptorSets, {});
	return *this;
}

template <>
void DescriptorSetArray<kVk>::Swap(DescriptorSetArray& rhs) noexcept
{
	DeviceObject::Swap(rhs);
	std::swap(myDesc, rhs.myDesc);
	std::swap(myDescriptorSets, rhs.myDescriptorSets);
}

template <>
void DescriptorUpdateTemplate<kVk>::InternalDestroyTemplate()
{
	ZoneScopedN("DescriptorSetLayout::vkDestroyDescriptorUpdateTemplate");

	vkDestroyDescriptorUpdateTemplate(
		*GetDevice(),
		myHandle,
		&GetDevice()->GetInstance()->GetHostAllocationCallbacks());
}

template <>
void DescriptorUpdateTemplate<kVk>::SetEntries(
	std::vector<DescriptorUpdateTemplateEntry<kVk>>&& entries)
{
	InternalDestroyTemplate();
	myEntries = std::forward<std::vector<DescriptorUpdateTemplateEntry<kVk>>>(entries);
	myHandle = [this]
	{
		VkDescriptorUpdateTemplate descriptorTemplate;
		VkDescriptorUpdateTemplateCreateInfo createInfo{
			VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO,
			nullptr,
			0, // reserved for future use
			static_cast<uint32_t>(myEntries.size()),
			myEntries.data(),
			GetDesc().templateType,
			GetDesc().descriptorSetLayout,
			GetDesc().pipelineBindPoint,
			GetDesc().pipelineLayout,
			GetDesc().set};
		vkCreateDescriptorUpdateTemplate(
			*GetDevice(),
			&createInfo,
			&GetDevice()->GetInstance()->GetHostAllocationCallbacks(),
			&descriptorTemplate);

		return descriptorTemplate;
	}();
}

template <>
DescriptorUpdateTemplate<kVk>::DescriptorUpdateTemplate(DescriptorUpdateTemplate&& other) noexcept
	: DeviceObject(std::forward<DescriptorUpdateTemplate>(other))
	, myDesc(std::exchange(other.myDesc, {}))
	, myHandle(std::exchange(other.myHandle, {}))
{}

template <>
DescriptorUpdateTemplate<kVk>::DescriptorUpdateTemplate(
	const std::shared_ptr<Device<kVk>>& device,
	DescriptorUpdateTemplateCreateDesc<kVk>&& desc,
	DescriptorUpdateTemplateHandle<kVk>&& handle)
	: DeviceObject(
		  device,
		  {"_DescriptorUpdateTemplate"},
		  1,
		  VK_OBJECT_TYPE_DESCRIPTOR_UPDATE_TEMPLATE,
		  reinterpret_cast<uint64_t*>(&handle))
	, myDesc(std::forward<DescriptorUpdateTemplateCreateDesc<kVk>>(desc))
	, myHandle(std::forward<DescriptorUpdateTemplateHandle<kVk>>(handle))
{}

template <>
DescriptorUpdateTemplate<kVk>::DescriptorUpdateTemplate(
	const std::shared_ptr<Device<kVk>>& device,
	DescriptorUpdateTemplateCreateDesc<kVk>&& desc)
	: DescriptorUpdateTemplate(
		  device, std::forward<DescriptorUpdateTemplateCreateDesc<kVk>>(desc), VK_NULL_HANDLE)
{}

template <>
DescriptorUpdateTemplate<kVk>::~DescriptorUpdateTemplate()
{
	if (myHandle != nullptr)
		InternalDestroyTemplate();
}

template <>
DescriptorUpdateTemplate<kVk>&
DescriptorUpdateTemplate<kVk>::operator=(DescriptorUpdateTemplate&& other) noexcept
{
	DeviceObject::operator=(std::forward<DescriptorUpdateTemplate>(other));
	myDesc = std::exchange(other.myDesc, {});
	myHandle = std::exchange(other.myHandle, {});
	return *this;
}

template <>
void DescriptorUpdateTemplate<kVk>::Swap(DescriptorUpdateTemplate& rhs) noexcept
{
	DeviceObject::Swap(rhs);
	std::swap(myDesc, rhs.myDesc);
	std::swap(myHandle, rhs.myHandle);
}
