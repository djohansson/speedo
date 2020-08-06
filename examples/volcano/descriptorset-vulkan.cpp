#include "descriptorset.h"
#include "vk-utils.h"

namespace descriptorset
{

std::vector<DescriptorSetLayoutHandle<Vk>> createDescriptorSetLayouts(
	DeviceHandle<Vk> device,
	const DescriptorSetLayoutBindingsMap<Vk>& bindings)
{
    std::vector<DescriptorSetLayoutHandle<Vk>> outLayouts;
    outLayouts.reserve(bindings.size());
	for (auto& [space, layoutBindings] : bindings)
	    outLayouts.emplace_back(createDescriptorSetLayout(device, layoutBindings.data(), layoutBindings.size()));
	return outLayouts;
}

}

template <>
DescriptorSetLayoutVector<Vk>::DescriptorSetLayoutVector(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    std::vector<DescriptorSetLayoutHandle<Vk>>&& descriptorSetLayoutVector)
: DeviceResource<Vk>(
    deviceContext,
    {"_DescriptorSetLayout"},
    descriptorSetLayoutVector.size(),
    VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT,
    reinterpret_cast<uint64_t*>(descriptorSetLayoutVector.data()))
, myDescriptorSetLayoutVector(std::move(descriptorSetLayoutVector))
{
}

template <>
DescriptorSetLayoutVector<Vk>::DescriptorSetLayoutVector(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    const DescriptorSetLayoutBindingsMap<Vk>& bindings)
: DescriptorSetLayoutVector<Vk>(
    deviceContext,
    descriptorset::createDescriptorSetLayouts(deviceContext->getDevice(), bindings))
{
}

template <>
DescriptorSetLayoutVector<Vk>::~DescriptorSetLayoutVector()
{
    if (myDescriptorSetLayoutVector.size())
        for (auto layout : myDescriptorSetLayoutVector)
            vkDestroyDescriptorSetLayout(getDeviceContext()->getDevice(), layout, nullptr);
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
    const DescriptorSetLayoutHandle<Vk>* layoutHandles,
    uint32_t layoutHandleCount)
: DescriptorSetVector<Vk>(
    deviceContext,
    allocateDescriptorSets(
        deviceContext->getDevice(),
        deviceContext->getDescriptorPool(),
        layoutHandles,
        layoutHandleCount))
{
}

template <>
DescriptorSetVector<Vk>::DescriptorSetVector(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    const DescriptorSetLayoutVector<Vk>& layouts)
: DescriptorSetVector<Vk>(
    deviceContext,
    layouts.data(),
    layouts.size())
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
