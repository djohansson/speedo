#include "descriptorset.h"
#include "vk-utils.h"

#include <memory>

#include <xxhash.h>

namespace descriptorsetlayout
{

uint64_t hash(const DescriptorSetLayoutCreateDesc<Vk>& desc)
{
    thread_local std::unique_ptr<XXH3_state_t, XXH_errorcode(*)(XXH3_state_t*)> threadXXHState =
    {
        XXH3_createState(),
        XXH3_freeState
    };

    auto result = XXH3_64bits_reset(threadXXHState.get());
    assert(result != XXH_ERROR);

    if (!desc.bindings.empty())
    {
        result = XXH3_64bits_update(
            threadXXHState.get(),
            desc.bindings.data(),
            desc.bindings.size() * sizeof(decltype(desc.bindings)::value_type));
        assert(result != XXH_ERROR);
    }

    if (!desc.variableNames.empty())
    {
        result = XXH3_64bits_update(
            threadXXHState.get(),
            desc.variableNameHashes.data(),
            desc.variableNameHashes.size() * sizeof(decltype(desc.variableNameHashes)::value_type));
        assert(result != XXH_ERROR);
    }

    if (desc.pushConstantRange.has_value())
    {
        result = XXH3_64bits_update(
            threadXXHState.get(),
            &desc.pushConstantRange.value(),
            sizeof(decltype(desc.pushConstantRange)::value_type));
        assert(result != XXH_ERROR);
    }

    result = XXH3_64bits_update(
        threadXXHState.get(),
        &desc.flags,
        sizeof(desc.flags));

    return XXH3_64bits_digest(threadXXHState.get());
}

}

template <>
DescriptorSetLayout<Vk>::DescriptorSetLayout(DescriptorSetLayout&& other) noexcept
: DeviceResource<Vk>(std::move(other))
, myDesc(std::exchange(other.myDesc, {}))
, myKey(std::exchange(other.myKey, 0))
, myLayout(std::exchange(other.myLayout, {}))
{
}

template <>
DescriptorSetLayout<Vk>::DescriptorSetLayout(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    DescriptorSetLayoutCreateDesc<Vk>&& desc,
    ValueType&& layout)
: DeviceResource<Vk>(
    deviceContext,
    {"_DescriptorSetLayout"},
    1,
    VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT,
    reinterpret_cast<uint64_t*>(&std::get<0>(layout)))
, myDesc(std::move(desc))
, myKey(descriptorsetlayout::hash(myDesc))
, myLayout(std::move(layout))
{
}

template <>
DescriptorSetLayout<Vk>::DescriptorSetLayout(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    DescriptorSetLayoutCreateDesc<Vk>&& desc)
: DescriptorSetLayout<Vk>(
    deviceContext,
    std::move(desc),
    [&deviceContext, &desc]
    {
        auto& bindingVector = desc.bindings;
        
        auto samplers = SamplerVector<Vk>(deviceContext, desc.immutableSamplers);
        BindingsMapType bindingsMap;

        for (size_t bindingIt = 0; bindingIt < bindingVector.size(); bindingIt++)
        {
            auto& binding = bindingVector[bindingIt];
            binding.pImmutableSamplers = samplers.data();
            bindingsMap.emplace(desc.variableNameHashes.at(bindingIt), binding.binding);
        }
        
        return std::make_tuple(
            createDescriptorSetLayout(
                deviceContext->getDevice(),
                desc.flags,
                bindingVector.data(),
                bindingVector.size()),
            std::move(samplers),
            std::move(bindingsMap));
    }())
{
}

template <>
DescriptorSetLayout<Vk>::~DescriptorSetLayout()
{
    if (auto layout = std::get<0>(myLayout); layout)
        vkDestroyDescriptorSetLayout(getDeviceContext()->getDevice(), layout, nullptr);
}

template <>
DescriptorSetLayout<Vk>& DescriptorSetLayout<Vk>::operator=(DescriptorSetLayout&& other) noexcept
{
    DeviceResource<Vk>::operator=(std::move(other));
    myDesc = std::exchange(other.myDesc, {});
    myKey = std::exchange(other.myKey, 0);
    myLayout = std::exchange(other.myLayout, {});
	return *this;
}

template <>
void DescriptorSetLayout<Vk>::swap(DescriptorSetLayout& rhs) noexcept
{
    DeviceResource<Vk>::swap(rhs);
    std::swap(myDesc, rhs.myDesc);
    std::swap(myKey, rhs.myKey);
    std::swap(myLayout, rhs.myLayout);
}

template <>
DescriptorSetArray<Vk>::DescriptorSetArray(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    DescriptorSetArrayCreateDesc<Vk>&& desc,
    ArrayType&& descriptorSetHandles)
: DeviceResource<Vk>(
    deviceContext,
    {"_DescriptorSet"},
    descriptorSetHandles.size(),
    VK_OBJECT_TYPE_DESCRIPTOR_SET,
    reinterpret_cast<uint64_t*>(descriptorSetHandles.data()))
, myDesc(std::move(desc))
, myDescriptorSets(std::move(descriptorSetHandles))
{
}

template <>
DescriptorSetArray<Vk>::DescriptorSetArray(DescriptorSetArray&& other) noexcept
: DeviceResource<Vk>(std::move(other))
, myDesc(std::exchange(other.myDesc, {}))
, myDescriptorSets(std::exchange(other.myDescriptorSets, {}))
{
}

template <>
DescriptorSetArray<Vk>::DescriptorSetArray(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    const DescriptorSetLayout<Vk>& layout,
    DescriptorSetArrayCreateDesc<Vk>&& desc)
: DescriptorSetArray<Vk>(
    deviceContext,
    std::move(desc),
    [device = deviceContext->getDevice(), &layout, &desc]
    {
        std::array<VkDescriptorSetLayout, kDescriptorSetCount> layouts;
        layouts.fill(layout);

        ArrayType sets;
        VkDescriptorSetAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        allocInfo.descriptorPool = desc.pool;
        allocInfo.descriptorSetCount = layouts.size();
        allocInfo.pSetLayouts = layouts.data();
        VK_CHECK(vkAllocateDescriptorSets(device, &allocInfo, sets.data()));

        return sets;
    }())
{
}

template <>
DescriptorSetArray<Vk>::~DescriptorSetArray()
{
    if (isValid())
        getDeviceContext()->addTimelineCallback(
            [device = getDeviceContext()->getDevice(), pool = myDesc.pool, descriptorSetHandles = std::move(myDescriptorSets)](uint64_t){
                vkFreeDescriptorSets(
                    device,
                    pool,
                    descriptorSetHandles.size(),
                    descriptorSetHandles.data());
        });
}

template <>
DescriptorSetArray<Vk>& DescriptorSetArray<Vk>::operator=(DescriptorSetArray&& other) noexcept
{
    DeviceResource<Vk>::operator=(std::move(other));
    myDesc = std::exchange(other.myDesc, {});
    myDescriptorSets = std::exchange(other.myDescriptorSets, {});
    return *this;
}

template <>
void DescriptorSetArray<Vk>::swap(DescriptorSetArray& rhs) noexcept
{
    DeviceResource<Vk>::swap(rhs);
    std::swap(myDesc, rhs.myDesc);
    std::swap(myDescriptorSets, rhs.myDescriptorSets);
}
