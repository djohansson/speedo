#include "descriptorset.h"
#include "vk-utils.h"

namespace descriptorset
{

static PFN_vkCmdPushDescriptorSetKHR vkCmdPushDescriptorSetKHR = {};

// template <>
// std::vector<DescriptorSetLayoutHandle<Vk>> getDescriptorSetLayoutHandles<Vk>(
//     const std::vector<DescriptorSetLayout<Vk>>& layouts,
//     bool nullIfPushConstant)
// {
//     std::vector<DescriptorSetLayoutHandle<Vk>> handles;
//     handles.reserve(layouts.size());
//     std::transform(
//         layouts.begin(),
//         layouts.end(),
//         std::back_inserter(handles),
//         [nullIfPushConstant](const auto& layout){
//             if (nullIfPushConstant && layout.getDesc().flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR)
//                 return DescriptorSetLayoutHandle<Vk>{};
//             return static_cast<DescriptorSetLayoutHandle<Vk>>(layout);});       
//     return handles;
// }

}

template <>
DescriptorSetLayout<Vk>::DescriptorSetLayout(DescriptorSetLayout<Vk>&& other)
: DeviceResource<Vk>(std::move(other))
, myDesc(std::exchange(other.myDesc, {}))
, myDescriptorSetLayout(std::exchange(other.myDescriptorSetLayout, {}))
{
}

template <>
DescriptorSetLayout<Vk>::DescriptorSetLayout(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    DescriptorSetLayoutCreateDesc<Vk>&& desc,
    DescriptorSetLayoutHandle<Vk>&& descriptorSetLayout)
: DeviceResource<Vk>(
    deviceContext,
    {"_DescriptorSetLayout"},
    1,
    VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT,
    reinterpret_cast<uint64_t*>(&descriptorSetLayout))
, myDesc(std::move(desc))
, myDescriptorSetLayout(std::move(descriptorSetLayout))
{
}

template <>
DescriptorSetLayout<Vk>::DescriptorSetLayout(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    DescriptorSetLayoutCreateDesc<Vk>&& desc,
    const std::vector<SerializableDescriptorSetLayoutBinding<Vk>>& bindings)
: DescriptorSetLayout<Vk>(
    deviceContext,
    std::move(desc),
    createDescriptorSetLayout(
        deviceContext->getDevice(),
        desc.flags,
        bindings.data(),
        bindings.size()))
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
    myDesc = std::exchange(other.myDesc, {});
    myDescriptorSetLayout = std::exchange(other.myDescriptorSetLayout, {});
	return *this;
}

template <>
void DescriptorSetVector<Vk>::copy(DescriptorSetVector<Vk>& dst) const
{
    std::vector<CopyDescriptorSet<Vk>> descriptorCopies;

    for (const auto& [set, bindingVector] : myData)
    {
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
    }

    vkUpdateDescriptorSets(
        getDeviceContext()->getDevice(),
        0,
        nullptr,
        static_cast<uint32_t>(descriptorCopies.size()),
        descriptorCopies.data());
}

template <>
void DescriptorSetVector<Vk>::push(
    CommandBufferHandle<Vk> cmd,
    PipelineBindPoint<Vk> bindPoint,
    PipelineLayoutHandle<Vk> layout,
    uint32_t set,
    uint32_t bindingOffset,
    uint32_t bindingCount) const
{
    auto setHandle = myDescriptorSets[set];
    const auto& bindingVector = myData.at(set);

    assert(bindingOffset + bindingCount <= bindingVector.size());

    std::vector<WriteDescriptorSet<Vk>> descriptorWrites;
    descriptorWrites.reserve(descriptorWrites.size() + bindingVector.size());

    for (uint32_t bindingIt = bindingOffset; bindingIt < bindingCount; bindingIt++)
    {
        const auto& binding = bindingVector[bindingIt];
        auto bindingType = std::get<0>(binding);
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

template <>
void DescriptorSetVector<Vk>::write() const
{
    std::vector<WriteDescriptorSet<Vk>> descriptorWrites;

    for (const auto& [set, bindingVector] : myData)
    {
        descriptorWrites.reserve(descriptorWrites.size() + bindingVector.size());

        for (const auto& binding : bindingVector)
        {
            auto setHandle = myDescriptorSets[set];

            if (!setHandle)
                continue;

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
    }

    vkUpdateDescriptorSets(
        getDeviceContext()->getDevice(),
        static_cast<uint32_t>(descriptorWrites.size()),
        descriptorWrites.data(),
        0,
        nullptr);
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
DescriptorSetVector<Vk>::DescriptorSetVector(const DescriptorSetVector<Vk>& other)
: DescriptorSetVector<Vk>(
    other.getDeviceContext(),
    DescriptorSetVectorCreateDesc<Vk>(other.getDesc()),
    [device = other.getDeviceContext()->getDevice(), desc = other.myDesc]
    {
        std::vector<DescriptorSetHandle<Vk>> sets;
        sets.resize(desc.layouts.size());
        for (auto layoutIt = 0; layoutIt < desc.layouts.size(); layoutIt++)
        {
            auto layout = desc.layouts[layoutIt];
            if (layout)
                sets[layoutIt] = allocateDescriptorSet(device, desc.pool, layout);
        }
        return sets;
    }())
{
    myData = other.myData;
    other.copy(*this);
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
    DescriptorSetVectorCreateDesc<Vk>&& desc)
: DescriptorSetVector<Vk>(
    deviceContext,
    std::move(desc),
    [device = deviceContext->getDevice(), &desc]
    {
        std::vector<DescriptorSetHandle<Vk>> sets;
        sets.resize(desc.layouts.size());
        for (auto layoutIt = 0; layoutIt < desc.layouts.size(); layoutIt++)
        {
            auto layout = desc.layouts[layoutIt];
            if (layout)
                sets[layoutIt] = allocateDescriptorSet(device, desc.pool, layout);
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
                // for (auto set : descriptorSets)
                //     vkFreeDescriptorSets(device, pool, 1, &set);
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


template <>
DescriptorSetVector<Vk>& DescriptorSetVector<Vk>::operator=(const DescriptorSetVector<Vk>& other)
{
    return *this = DescriptorSetVector<Vk>(other);
}
