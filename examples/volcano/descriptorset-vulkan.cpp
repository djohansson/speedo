#include "descriptorset.h"
#include "vk-utils.h"

template <>
DescriptorSetLayout<Vk>::DescriptorSetLayout(DescriptorSetLayout<Vk>&& other)
: DeviceResource<Vk>(std::move(other))
, myDesc(std::exchange(other.myDesc, {}))
, myLayout(std::move(other.myLayout))
{
    std::get<0>(other.myLayout) = {};
}

template <>
DescriptorSetLayout<Vk>::DescriptorSetLayout(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    DescriptorSetLayoutCreateDesc<Vk>&& desc,
    std::tuple<DescriptorSetLayoutHandle<Vk>, SamplerVector<Vk>>&& layout)
: DeviceResource<Vk>(
    deviceContext,
    {"_DescriptorSetLayout"},
    1,
    VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT,
    reinterpret_cast<uint64_t*>(&std::get<0>(layout)))
, myDesc(std::move(desc))
, myLayout(std::move(layout))
{
}

template <>
DescriptorSetLayout<Vk>::DescriptorSetLayout(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    DescriptorSetLayoutCreateDesc<Vk>&& desc,
    const std::vector<SamplerCreateInfo<Vk>>& immutableSamplers,
    std::vector<DescriptorSetLayoutBinding<Vk>>& bindings)
: DescriptorSetLayout<Vk>(
    deviceContext,
    std::move(desc),
    [&deviceContext, &desc, &immutableSamplers, &bindings]
    {
        auto samplers = SamplerVector<Vk>(deviceContext, immutableSamplers);

        for (auto& binding : bindings)
            binding.pImmutableSamplers = samplers.data();

        return std::make_tuple(
            createDescriptorSetLayout(
                deviceContext->getDevice(),
                desc.flags,
                bindings.data(),
                bindings.size()),
            std::move(samplers));
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
DescriptorSetLayout<Vk>& DescriptorSetLayout<Vk>::operator=(DescriptorSetLayout<Vk>&& other)
{
    DeviceResource<Vk>::operator=(std::move(other));
    myDesc = std::exchange(other.myDesc, {});
    std::get<0>(myLayout) = std::exchange(std::get<0>(other.myLayout), {});
    std::get<1>(myLayout) = std::move(std::get<1>(other.myLayout));
	return *this;
}

template <>
DescriptorSetVector<Vk>::DescriptorSetVector(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    DescriptorSetVectorCreateDesc<Vk>&& desc,
    std::vector<DescriptorSetHandle<Vk>>&& descriptorSetHandles)
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
DescriptorSetVector<Vk>::DescriptorSetVector(DescriptorSetVector<Vk>&& other)
: DeviceResource<Vk>(std::move(other))
, myDesc(std::exchange(other.myDesc, {}))
, myDescriptorSets(std::exchange(other.myDescriptorSets, {}))
{
}

template <>
DescriptorSetVector<Vk>::DescriptorSetVector(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    const std::vector<DescriptorSetLayout<Vk>>& layouts,
    DescriptorSetVectorCreateDesc<Vk>&& desc)
: DescriptorSetVector<Vk>(
    deviceContext,
    std::move(desc),
    [device = deviceContext->getDevice(), &layouts, &desc]
    {
        std::vector<DescriptorSetHandle<Vk>> sets;
        sets.reserve(layouts.size());

        for (const auto& layout : layouts)
        {
            if (layout.getDesc().flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR)
                sets.emplace_back(DescriptorSetHandle<Vk>{});
            else
                sets.emplace_back(allocateDescriptorSet(device, desc.pool, layout));
        }

        return sets;
    }())
{
}

template <>
DescriptorSetVector<Vk>::~DescriptorSetVector()
{
    if (!myDescriptorSets.empty())
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
DescriptorSetVector<Vk>& DescriptorSetVector<Vk>::operator=(DescriptorSetVector<Vk>&& other)
{
    DeviceResource<Vk>::operator=(std::move(other));
    myDesc = std::exchange(other.myDesc, {});
    myDescriptorSets = std::exchange(other.myDescriptorSets, {});
    return *this;
}
