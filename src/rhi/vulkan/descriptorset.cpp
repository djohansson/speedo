#include "../descriptorset.h"

#include "utils.h"

#include <utility>

template <>
DescriptorSetLayout<Vk>::DescriptorSetLayout(DescriptorSetLayout&& other) noexcept
	: DeviceObject(std::forward<DescriptorSetLayout>(other))
	, myDesc(std::exchange(other.myDesc, {}))
	, myLayout(std::exchange(other.myLayout, {}))
{}

template <>
DescriptorSetLayout<Vk>::DescriptorSetLayout(
	const std::shared_ptr<Device<Vk>>& device,
	DescriptorSetLayoutCreateDesc<Vk>&& desc,
	ValueType&& layout)
	: DeviceObject(
		  device,
		  {"_DescriptorSetLayout"},
		  1,
		  VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT,
		  reinterpret_cast<uint64_t*>(&std::get<0>(layout)))
	, myDesc(std::forward<DescriptorSetLayoutCreateDesc<Vk>>(desc))
	, myLayout(std::forward<ValueType>(layout))
{}

template <>
DescriptorSetLayout<Vk>::DescriptorSetLayout(
	const std::shared_ptr<Device<Vk>>& device,
	DescriptorSetLayoutCreateDesc<Vk>&& desc)
	: DescriptorSetLayout(
		  device,
		  std::forward<DescriptorSetLayoutCreateDesc<Vk>>(desc),
		  [&device, &desc]
		  {
			  ZoneScopedN("DescriptorSetLayout::createDescriptorSetLayout");

			  auto samplers = SamplerVector<Vk>(device, desc.immutableSamplers);

			  ShaderVariableBindingsMap bindingsMap;
			  auto& bindings = desc.bindings;
			  for (size_t bindingIt = 0; bindingIt < bindings.size(); bindingIt++)
			  {
				  auto& binding = bindings[bindingIt];
				  binding.pImmutableSamplers = samplers.data();
				  bindingsMap.emplace(
					  desc.variableNameHashes.at(bindingIt),
					  std::make_tuple(
						  binding.binding, binding.descriptorType, binding.descriptorCount));
			  }

			  auto& bindingFlags = desc.bindingFlags;

			  ASSERT(bindings.size() == bindingFlags.size());

			  auto* layout = createDescriptorSetLayout(
				  *device,
				  &device->getInstance()->getHostAllocationCallbacks(),
				  desc.flags,
				  bindings.data(),
				  bindingFlags.data(),
				  bindings.size());

			  return std::make_tuple(layout, std::move(samplers), std::move(bindingsMap));
		  }())
{}

template <>
DescriptorSetLayout<Vk>::~DescriptorSetLayout()
{
	if (auto* layout = std::get<0>(myLayout); layout)
	{
		ZoneScopedN("DescriptorSetLayout::vkDestroyDescriptorSetLayout");

		vkDestroyDescriptorSetLayout(
			*getDevice(),
			layout,
			&getDevice()->getInstance()->getHostAllocationCallbacks());
	}
}

template <>
DescriptorSetLayout<Vk>& DescriptorSetLayout<Vk>::operator=(DescriptorSetLayout&& other) noexcept
{
	DeviceObject::operator=(std::forward<DescriptorSetLayout>(other));
	myDesc = std::exchange(other.myDesc, {});
	myLayout = std::exchange(other.myLayout, {});
	return *this;
}

template <>
void DescriptorSetLayout<Vk>::Swap(DescriptorSetLayout& rhs) noexcept
{
	DeviceObject::Swap(rhs);
	std::swap(myDesc, rhs.myDesc);
	std::swap(myLayout, rhs.myLayout);
}

template <>
DescriptorSetArray<Vk>::DescriptorSetArray(
	const std::shared_ptr<Device<Vk>>& device,
	DescriptorSetArrayCreateDesc<Vk>&& desc,
	ArrayType&& descriptorSetHandles)
	: DeviceObject(
		  device,
		  {"_DescriptorSet"},
		  descriptorSetHandles.size(),
		  VK_OBJECT_TYPE_DESCRIPTOR_SET,
		  reinterpret_cast<uint64_t*>(descriptorSetHandles.data()))
	, myDesc(std::forward<DescriptorSetArrayCreateDesc<Vk>>(desc))
	, myDescriptorSets(std::forward<ArrayType>(descriptorSetHandles))
{}

template <>
DescriptorSetArray<Vk>::DescriptorSetArray(DescriptorSetArray&& other) noexcept
	: DeviceObject(std::forward<DescriptorSetArray>(other))
	, myDesc(std::exchange(other.myDesc, {}))
	, myDescriptorSets(std::exchange(other.myDescriptorSets, {}))
{}

template <>
DescriptorSetArray<Vk>::DescriptorSetArray(
	const std::shared_ptr<Device<Vk>>& device,
	const DescriptorSetLayout<Vk>& layout,
	DescriptorSetArrayCreateDesc<Vk>&& desc)
	: DescriptorSetArray(
		  device,
		  std::forward<DescriptorSetArrayCreateDesc<Vk>>(desc),
		  [&device, &layout, &desc]
		  {
			  ZoneScopedN("DescriptorSetArray::vkAllocateDescriptorSets");

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
DescriptorSetArray<Vk>::~DescriptorSetArray()
{
	if (isValid())
		vkFreeDescriptorSets(*getDevice(), myDesc.pool, myDescriptorSets.size(), myDescriptorSets.data());
}

template <>
DescriptorSetArray<Vk>& DescriptorSetArray<Vk>::operator=(DescriptorSetArray&& other) noexcept
{
	DeviceObject::operator=(std::forward<DescriptorSetArray>(other));
	myDesc = std::exchange(other.myDesc, {});
	myDescriptorSets = std::exchange(other.myDescriptorSets, {});
	return *this;
}

template <>
void DescriptorSetArray<Vk>::Swap(DescriptorSetArray& rhs) noexcept
{
	DeviceObject::Swap(rhs);
	std::swap(myDesc, rhs.myDesc);
	std::swap(myDescriptorSets, rhs.myDescriptorSets);
}

template <>
void DescriptorUpdateTemplate<Vk>::InternalDestroyTemplate()
{
	ZoneScopedN("DescriptorSetLayout::vkDestroyDescriptorUpdateTemplate");

	vkDestroyDescriptorUpdateTemplate(
		*getDevice(),
		myHandle,
		&getDevice()->getInstance()->getHostAllocationCallbacks());
}

template <>
void DescriptorUpdateTemplate<Vk>::SetEntries(
	std::vector<DescriptorUpdateTemplateEntry<Vk>>&& entries)
{
	InternalDestroyTemplate();
	myEntries = std::forward<std::vector<DescriptorUpdateTemplateEntry<Vk>>>(entries);
	myHandle = [this]
	{
		ZoneScopedN("DescriptorSetLayout::vkCreateDescriptorUpdateTemplate");

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
			*getDevice(),
			&createInfo,
			&getDevice()->getInstance()->getHostAllocationCallbacks(),
			&descriptorTemplate);

		return descriptorTemplate;
	}();
}

template <>
DescriptorUpdateTemplate<Vk>::DescriptorUpdateTemplate(DescriptorUpdateTemplate&& other) noexcept
	: DeviceObject(std::forward<DescriptorUpdateTemplate>(other))
	, myDesc(std::exchange(other.myDesc, {}))
	, myHandle(std::exchange(other.myHandle, {}))
{}

template <>
DescriptorUpdateTemplate<Vk>::DescriptorUpdateTemplate(
	const std::shared_ptr<Device<Vk>>& device,
	DescriptorUpdateTemplateCreateDesc<Vk>&& desc,
	DescriptorUpdateTemplateHandle<Vk>&& handle)
	: DeviceObject(
		  device,
		  {"_DescriptorUpdateTemplate"},
		  1,
		  VK_OBJECT_TYPE_DESCRIPTOR_UPDATE_TEMPLATE,
		  reinterpret_cast<uint64_t*>(&handle))
	, myDesc(std::forward<DescriptorUpdateTemplateCreateDesc<Vk>>(desc))
	, myHandle(std::forward<DescriptorUpdateTemplateHandle<Vk>>(handle))
{}

template <>
DescriptorUpdateTemplate<Vk>::DescriptorUpdateTemplate(
	const std::shared_ptr<Device<Vk>>& device,
	DescriptorUpdateTemplateCreateDesc<Vk>&& desc)
	: DescriptorUpdateTemplate(
		  device, std::forward<DescriptorUpdateTemplateCreateDesc<Vk>>(desc), VK_NULL_HANDLE)
{}

template <>
DescriptorUpdateTemplate<Vk>::~DescriptorUpdateTemplate()
{
	if (myHandle != nullptr)
		InternalDestroyTemplate();
}

template <>
DescriptorUpdateTemplate<Vk>&
DescriptorUpdateTemplate<Vk>::operator=(DescriptorUpdateTemplate&& other) noexcept
{
	DeviceObject::operator=(std::forward<DescriptorUpdateTemplate>(other));
	myDesc = std::exchange(other.myDesc, {});
	myHandle = std::exchange(other.myHandle, {});
	return *this;
}

template <>
void DescriptorUpdateTemplate<Vk>::Swap(DescriptorUpdateTemplate& rhs) noexcept
{
	DeviceObject::Swap(rhs);
	std::swap(myDesc, rhs.myDesc);
	std::swap(myHandle, rhs.myHandle);
}
