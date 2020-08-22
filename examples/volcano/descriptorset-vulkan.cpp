#include "descriptorset.h"
#include "vk-utils.h"

template <>
DescriptorSetLayout<Vk>::DescriptorSetLayout(DescriptorSetLayout<Vk>&& other)
: DeviceResource<Vk>(std::move(other))
, myDescriptorSetLayout(std::exchange(other.myDescriptorSetLayout, {}))
{
}

template <>
DescriptorSetLayout<Vk>::DescriptorSetLayout(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    DescriptorSetLayoutHandle<Vk>&& descriptorSetLayout)
: DeviceResource<Vk>(
    deviceContext,
    {"_DescriptorSetLayout"},
    1,
    VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT,
    reinterpret_cast<uint64_t*>(&descriptorSetLayout))
, myDescriptorSetLayout(std::move(descriptorSetLayout))
{
}

template <>
DescriptorSetLayout<Vk>::DescriptorSetLayout(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    const std::vector<SerializableDescriptorSetLayoutBinding<Vk>>& bindings)
: DescriptorSetLayout<Vk>(
    deviceContext,
    createDescriptorSetLayout(deviceContext->getDevice(), bindings.data(), bindings.size()))
{
}

template <>
DescriptorSetLayout<Vk>::~DescriptorSetLayout()
{
    if (myDescriptorSetLayout)
        vkDestroyDescriptorSetLayout(getDeviceContext()->getDevice(), myDescriptorSetLayout, nullptr);
}

template <>
DescriptorSetLayout<Vk>& DescriptorSetLayout<Vk>::operator=(DescriptorSetLayout<Vk>&& other)
{
	DeviceResource<Vk>::operator=(std::move(other));
	myDescriptorSetLayout = std::exchange(other.myDescriptorSetLayout, {});
	return *this;
}


template <>
DescriptorSetVector<Vk>::DescriptorSetVector(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    std::vector<DescriptorSetHandle<Vk>>&& descriptorSetVector)
: DeviceResource<Vk>(
    deviceContext,
    {"_DescriptorSet"},
    descriptorSetVector.size(),
    VK_OBJECT_TYPE_DESCRIPTOR_SET,
    reinterpret_cast<uint64_t*>(descriptorSetVector.data()))
, myDescriptorSetVector(std::move(descriptorSetVector))
{
}


template <>
DescriptorSetVector<Vk>::DescriptorSetVector(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    const std::vector<DescriptorSetLayout<Vk>>& layouts)
: DescriptorSetVector<Vk>(
    deviceContext,
    [&deviceContext, &layouts]
    {
        std::vector<DescriptorSetLayoutHandle<Vk>> handles;
        handles.reserve(layouts.size());
        for (const auto& layout : layouts)
            handles.emplace_back(layout);
        return allocateDescriptorSets(
            deviceContext->getDevice(),
            deviceContext->getDescriptorPool(),
            handles.data(),
            handles.size());
    }())
{
}

template <>
DescriptorSetVector<Vk>::~DescriptorSetVector()
{
    if (!myDescriptorSetVector.empty())
        getDeviceContext()->addTimelineCallback(
            [device = getDeviceContext()->getDevice(), pool = getDeviceContext()->getDescriptorPool(), descriptorSetHandles = std::move(myDescriptorSetVector)](uint64_t){
                vkFreeDescriptorSets(
                    device,
                    pool,
                    descriptorSetHandles.size(),
                    descriptorSetHandles.data());
        });
}
