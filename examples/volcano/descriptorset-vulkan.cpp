#include "descriptorset.h"
#include "vk-utils.h"

namespace descriptorset
{

static PFN_vkCmdPushDescriptorSetKHR vkCmdPushDescriptorSetKHR = {};

}

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
void DescriptorSetVector<Vk>::copy(uint32_t set, DescriptorSetVector<Vk>& dst) const
{
    if (const auto& bindingVector = myData.at(set); !bindingVector.empty())
    {
        std::vector<CopyDescriptorSet<Vk>> descriptorCopies;
        descriptorCopies.reserve(descriptorCopies.size() + bindingVector.size());

        for (const auto& binding : bindingVector)
        {
            auto srcSetHandle = myDescriptorSets[set];
            auto dstSetHandle = dst[set];
            auto bindingIt = static_cast<uint32_t>(&binding - &bindingVector[0]);
            const auto& variantVector = std::get<1>(binding);

            descriptorCopies.emplace_back(std::visit(descriptorset::overloaded{
                [srcSetHandle, dstSetHandle, bindingIt](
                    const std::vector<DescriptorBufferInfo<Vk>>& bufferInfos){
                        return CopyDescriptorSet<Vk>{
                            VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET,
                            nullptr,
                            srcSetHandle,
                            bindingIt,
                            0,
                            dstSetHandle,
                            bindingIt,
                            0,
                            static_cast<uint32_t>(bufferInfos.size())};
                },
                [srcSetHandle, dstSetHandle, bindingIt](
                    const std::vector<DescriptorImageInfo<Vk>>& imageInfos){
                        return CopyDescriptorSet<Vk>{
                            VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET,
                            nullptr,
                            srcSetHandle,
                            bindingIt,
                            0,
                            dstSetHandle,
                            bindingIt,
                            0,
                            static_cast<uint32_t>(imageInfos.size())};
                },
                [srcSetHandle, dstSetHandle, bindingIt](
                    const std::vector<BufferViewHandle<Vk>>& bufferViews){
                        return CopyDescriptorSet<Vk>{
                            VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET,
                            nullptr,
                            srcSetHandle,
                            bindingIt,
                            0,
                            dstSetHandle,
                            bindingIt,
                            0,
                            static_cast<uint32_t>(bufferViews.size())};
                },
            }, variantVector));
        }

        vkUpdateDescriptorSets(
            getDeviceContext()->getDevice(),
            0,
            nullptr,
            static_cast<uint32_t>(descriptorCopies.size()),
            descriptorCopies.data());
    }
}

template <>
void DescriptorSetVector<Vk>::push(
    CommandBufferHandle<Vk> cmd,
    PipelineBindPoint<Vk> bindPoint,
    PipelineLayoutHandle<Vk> layout,
    uint32_t set) const
{
    if (const auto& bindingVector = myData.at(set); !bindingVector.empty())
    {
        std::vector<WriteDescriptorSet<Vk>> descriptorWrites;
        descriptorWrites.reserve(descriptorWrites.size() + bindingVector.size());

        for (uint32_t bindingIt = 0; bindingIt < bindingVector.size(); bindingIt++)
        {
            const auto& binding = bindingVector[bindingIt];
            auto bindingType = std::get<0>(binding);
            const auto& variantVector = std::get<1>(binding);

            descriptorWrites.emplace_back(std::visit(descriptorset::overloaded{
                [bindingType, bindingIt](
                const std::vector<DescriptorBufferInfo<Vk>>& bufferInfos){
                    return WriteDescriptorSet<Vk>{
                        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                        nullptr,
                        0,
                        bindingIt,
                        0,
                        static_cast<uint32_t>(bufferInfos.size()),
                        bindingType,
                        nullptr,
                        bufferInfos.data(),
                        nullptr};
                },
                [bindingType, bindingIt](
                const std::vector<DescriptorImageInfo<Vk>>& imageInfos){
                    return WriteDescriptorSet<Vk>{
                        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                        nullptr,
                        0,
                        bindingIt,
                        0,
                        static_cast<uint32_t>(imageInfos.size()),
                        bindingType,
                        imageInfos.data(),
                        nullptr,
                        nullptr};
                },
                [bindingType, bindingIt](
                const std::vector<BufferViewHandle<Vk>>& bufferViews){
                    return WriteDescriptorSet<Vk>{
                        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                        nullptr,
                        0,
                        bindingIt,
                        0,
                        static_cast<uint32_t>(bufferViews.size()),
                        bindingType,
                        nullptr,
                        nullptr,
                        bufferViews.data()};
                },
            }, variantVector));
        }

        if (!descriptorset::vkCmdPushDescriptorSetKHR)
            descriptorset::vkCmdPushDescriptorSetKHR = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(
                vkGetDeviceProcAddr(
                    getDeviceContext()->getDevice(),
                    "vkCmdPushDescriptorSetKHR"));

        descriptorset::vkCmdPushDescriptorSetKHR(
            cmd,
            bindPoint,
            layout,
            set,
            static_cast<uint32_t>(descriptorWrites.size()),
            descriptorWrites.data());
    }
}

template <>
void DescriptorSetVector<Vk>::write(uint32_t set) const
{
    if (const auto& bindingVector = myData.at(set); !bindingVector.empty())
    {
        std::vector<WriteDescriptorSet<Vk>> descriptorWrites;
        descriptorWrites.reserve(descriptorWrites.size() + bindingVector.size());

        for (const auto& binding : bindingVector)
        {
            auto setHandle = myDescriptorSets[set];
            auto bindingType = std::get<0>(binding);
            auto bindingIt = static_cast<uint32_t>(&binding - &bindingVector[0]);
            const auto& variantVector = std::get<1>(binding);

            descriptorWrites.emplace_back(std::visit(descriptorset::overloaded{
                [setHandle, bindingType, bindingIt](
                    const std::vector<DescriptorBufferInfo<Vk>>& bufferInfos){
                        return WriteDescriptorSet<Vk>{
                            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                            nullptr,
                            setHandle,
                            bindingIt,
                            0,
                            static_cast<uint32_t>(bufferInfos.size()),
                            bindingType,
                            nullptr,
                            bufferInfos.data(),
                            nullptr};
                },
                [setHandle, bindingType, bindingIt](
                    const std::vector<DescriptorImageInfo<Vk>>& imageInfos){
                        return WriteDescriptorSet<Vk>{
                            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                            nullptr,
                            setHandle,
                            bindingIt,
                            0,
                            static_cast<uint32_t>(imageInfos.size()),
                            bindingType,
                            imageInfos.data(),
                            nullptr,
                            nullptr};
                },
                [setHandle, bindingType, bindingIt](
                    const std::vector<BufferViewHandle<Vk>>& bufferViews){
                        return WriteDescriptorSet<Vk>{
                            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                            nullptr,
                            setHandle,
                            bindingIt,
                            0,
                            static_cast<uint32_t>(bufferViews.size()),
                            bindingType,
                            nullptr,
                            nullptr,
                            bufferViews.data()};
                },
            }, variantVector));
        }

        vkUpdateDescriptorSets(
            getDeviceContext()->getDevice(),
            static_cast<uint32_t>(descriptorWrites.size()),
            descriptorWrites.data(),
            0,
            nullptr);
    }
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
, myData(std::exchange(other.myData, {}))
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
    myData = std::exchange(other.myData, {});
    return *this;
}
