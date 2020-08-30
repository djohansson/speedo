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
void DescriptorSetVector<Vk>::update()
{
    myDescriptorWrites.clear();

    for (const auto& [setIndex, bindingVector] : myData)
    {
        for (const auto& binding : bindingVector)
        {
            auto set = myDescriptorSets[setIndex];
            auto bindingType = std::get<0>(binding);
            auto bindingIndex = static_cast<uint32_t>(&binding - &bindingVector[0]);
            const auto& variantVector = std::get<1>(binding);

            myDescriptorWrites.emplace_back(std::visit(descriptorset::overloaded{
                [set, bindingType, bindingIndex](
                    const std::vector<DescriptorBufferInfo<Vk>>& bufferInfos){
                        return WriteDescriptorSet<Vk>{
                            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                            nullptr,
                            set,
                            bindingIndex,
                            0,
                            static_cast<uint32_t>(bufferInfos.size()),
                            bindingType,
                            nullptr,
                            bufferInfos.data(),
                            nullptr};
                },
                [set, bindingType, bindingIndex](
                    const std::vector<DescriptorImageInfo<Vk>>& imageInfos){
                        return WriteDescriptorSet<Vk>{
                            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                            nullptr,
                            set,
                            bindingIndex,
                            0,
                            static_cast<uint32_t>(imageInfos.size()),
                            bindingType,
                            imageInfos.data(),
                            nullptr,
                            nullptr};
                },
                [set, bindingType, bindingIndex](
                    const std::vector<BufferViewHandle<Vk>>& bufferViews){
                        return WriteDescriptorSet<Vk>{
                            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                            nullptr,
                            set,
                            bindingIndex,
                            0,
                            static_cast<uint32_t>(bufferViews.size()),
                            bindingType,
                            nullptr,
                            nullptr,
                            bufferViews.data()};
                },
            }, variantVector));
        }
    }

    vkUpdateDescriptorSets(
        getDeviceContext()->getDevice(),
        static_cast<uint32_t>(myDescriptorWrites.size()),
        myDescriptorWrites.data(),
        0,
        nullptr);
}

template <>
DescriptorSetVector<Vk>::DescriptorSetVector(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    std::vector<DescriptorSetHandle<Vk>>&& descriptorSets)
: DeviceResource<Vk>(
    deviceContext,
    {"_DescriptorSet"},
    descriptorSets.size(),
    VK_OBJECT_TYPE_DESCRIPTOR_SET,
    reinterpret_cast<uint64_t*>(descriptorSets.data()))
, myDescriptorSets(std::move(descriptorSets))
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
    if (!myDescriptorSets.empty())
        getDeviceContext()->addTimelineCallback(
            [device = getDeviceContext()->getDevice(), pool = getDeviceContext()->getDescriptorPool(), descriptorSetHandles = std::move(myDescriptorSets)](uint64_t){
                vkFreeDescriptorSets(
                    device,
                    pool,
                    descriptorSetHandles.size(),
                    descriptorSetHandles.data());
        });
}
