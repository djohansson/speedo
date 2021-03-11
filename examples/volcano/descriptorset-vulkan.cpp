#include "descriptorset.h"
#include "vk-utils.h"

#include <memory>

#include <xxhash.h>

template <>
DescriptorSetLayout<Vk>::DescriptorSetLayout(DescriptorSetLayout&& other) noexcept
: DeviceResource(std::move(other))
, myDesc(std::exchange(other.myDesc, {}))
, myLayout(std::exchange(other.myLayout, {}))
{
}

template <>
DescriptorSetLayout<Vk>::DescriptorSetLayout(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    DescriptorSetLayoutCreateDesc<Vk>&& desc,
    ValueType&& layout)
: DeviceResource(
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
    DescriptorSetLayoutCreateDesc<Vk>&& desc)
: DescriptorSetLayout(
    deviceContext,
    std::move(desc),
    [&deviceContext, &desc]
    {
        ZoneScopedN("DescriptorSetLayout::createDescriptorSetLayout");

        auto samplers = SamplerVector<Vk>(deviceContext, desc.immutableSamplers);
        
        ShaderVariableBindingsMap<Vk> bindingsMap;
        auto& bindings = desc.bindings;
        for (size_t bindingIt = 0; bindingIt < bindings.size(); bindingIt++)
        {
            auto& binding = bindings[bindingIt];
            binding.pImmutableSamplers = samplers.data();
            bindingsMap.emplace(desc.variableNameHashes.at(bindingIt), std::make_tuple(binding.descriptorType, binding.binding));
        }

        auto& bindingFlags = desc.bindingFlags;

        assert(bindings.size() == bindingFlags.size());

        auto layout = createDescriptorSetLayout(
            deviceContext->getDevice(),
            desc.flags,
            bindings.data(),
            bindingFlags.data(),
            bindings.size());

        // typedef struct VkDescriptorUpdateTemplateCreateInfo {
        //     VkStructureType                           sType;
        //     const void*                               pNext;
        //     VkDescriptorUpdateTemplateCreateFlags     flags;
        //     uint32_t                                  descriptorUpdateEntryCount;
        //     const VkDescriptorUpdateTemplateEntry*    pDescriptorUpdateEntries;
        //     VkDescriptorUpdateTemplateType            templateType;
        //     VkDescriptorSetLayout                     descriptorSetLayout;
        //     VkPipelineBindPoint                       pipelineBindPoint;
        //     VkPipelineLayout                          pipelineLayout;
        //     uint32_t                                  set;
        // } VkDescriptorUpdateTemplateCreateInfo;
        
        // auto descriptorTemplate = createDescriptorUpdateTemplate(
        //     deviceContext->getDevice(),
        //     VkDescriptorUpdateTemplateCreateInfo
        //     {
        //         VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO,
        //         nullptr,
        //         0, // reserved for future use
        //         0,
        //         nullptr,
        //         VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET/* | VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR*/,
        //         layout,
        //         VkPipelineBindPoint{},
        //         VkPipelineLayout{},
        //         desc.set
        //     });
        
        return std::make_tuple(layout, std::move(samplers), std::move(bindingsMap));
    }())
{
}

template <>
DescriptorSetLayout<Vk>::~DescriptorSetLayout()
{
    if (auto layout = std::get<0>(myLayout); layout)
    {
        ZoneScopedN("DescriptorSetLayout::vkDestroyDescriptorSetLayout");
        
        vkDestroyDescriptorSetLayout(getDeviceContext()->getDevice(), layout, nullptr);
    }

    // if (auto descriptorTemplate = std::get<1>(myLayout); descriptorTemplate)
    // {
    //     ZoneScopedN("DescriptorSetLayout::vkDestroyDescriptorUpdateTemplate");
        
    //     vkDestroyDescriptorUpdateTemplate(getDeviceContext()->getDevice(), descriptorTemplate, nullptr);
    // }
}

template <>
DescriptorSetLayout<Vk>& DescriptorSetLayout<Vk>::operator=(DescriptorSetLayout&& other) noexcept
{
    DeviceResource::operator=(std::move(other));
    myDesc = std::exchange(other.myDesc, {});
    myLayout = std::exchange(other.myLayout, {});
	return *this;
}

template <>
void DescriptorSetLayout<Vk>::swap(DescriptorSetLayout& rhs) noexcept
{
    DeviceResource::swap(rhs);
    std::swap(myDesc, rhs.myDesc);
    std::swap(myLayout, rhs.myLayout);
}

template <>
DescriptorSetArray<Vk>::DescriptorSetArray(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    DescriptorSetArrayCreateDesc<Vk>&& desc,
    ArrayType&& descriptorSetHandles)
: DeviceResource(
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
: DeviceResource(std::move(other))
, myDesc(std::exchange(other.myDesc, {}))
, myDescriptorSets(std::exchange(other.myDescriptorSets, {}))
{
}

template <>
DescriptorSetArray<Vk>::DescriptorSetArray(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    const DescriptorSetLayout<Vk>& layout,
    DescriptorSetArrayCreateDesc<Vk>&& desc)
: DescriptorSetArray(
    deviceContext,
    std::move(desc),
    [device = deviceContext->getDevice(), &layout, &desc]
    {
        ZoneScopedN("DescriptorSetArray::vkAllocateDescriptorSets");

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
            [device = getDeviceContext()->getDevice(), pool = myDesc.pool, descriptorSetHandles = std::move(myDescriptorSets)](uint64_t)
            {
                ZoneScopedN("DescriptorSetArray::vkFreeDescriptorSets");
                
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
    DeviceResource::operator=(std::move(other));
    myDesc = std::exchange(other.myDesc, {});
    myDescriptorSets = std::exchange(other.myDescriptorSets, {});
    return *this;
}

template <>
void DescriptorSetArray<Vk>::swap(DescriptorSetArray& rhs) noexcept
{
    DeviceResource::swap(rhs);
    std::swap(myDesc, rhs.myDesc);
    std::swap(myDescriptorSets, rhs.myDescriptorSets);
}
