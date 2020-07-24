#include "descriptorset.h"
#include "vk-utils.h"


template <>
DescriptorSetLayout<GraphicsBackend::Vulkan>::DescriptorSetLayout(
    const std::shared_ptr<DeviceContext<GraphicsBackend::Vulkan>>& deviceContext,
    DescriptorSetLayoutHandle<GraphicsBackend::Vulkan>&& descriptorSetLayout)
: DeviceResource<GraphicsBackend::Vulkan>(
    deviceContext,
    {"_DescriptorSetLayout"},
    1,
    VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT,
    reinterpret_cast<uint64_t*>(&descriptorSetLayout))
, myDescriptorSetLayout(std::move(descriptorSetLayout))
{
}

template <>
DescriptorSetLayout<GraphicsBackend::Vulkan>::DescriptorSetLayout(
    const std::shared_ptr<DeviceContext<GraphicsBackend::Vulkan>>& deviceContext,
    const std::vector<DescriptorSetLayoutBinding<GraphicsBackend::Vulkan>>& bindings)
: DescriptorSetLayout<GraphicsBackend::Vulkan>(
    deviceContext,
    createDescriptorSetLayout(
        deviceContext->getDevice(),
        bindings.data(),
        bindings.size()))
{
}

template <>
DescriptorSetLayout<GraphicsBackend::Vulkan>::~DescriptorSetLayout()
{
    if (myDescriptorSetLayout)
        vkDestroyDescriptorSetLayout(getDeviceContext()->getDevice(), myDescriptorSetLayout, nullptr);
}

template <>
DescriptorSetVector<GraphicsBackend::Vulkan>::DescriptorSetVector(
    const std::shared_ptr<DeviceContext<GraphicsBackend::Vulkan>>& deviceContext,
    std::vector<DescriptorSetHandle<GraphicsBackend::Vulkan>>&& descriptorSetVector)
: DeviceResource<GraphicsBackend::Vulkan>(
    deviceContext,
    {"_DescriptorSet"},
    descriptorSetVector.size(),
    VK_OBJECT_TYPE_DESCRIPTOR_SET,
    reinterpret_cast<uint64_t*>(descriptorSetVector.data()))
, myDescriptorSetVector(std::move(descriptorSetVector))
{
}

template <>
DescriptorSetVector<GraphicsBackend::Vulkan>::DescriptorSetVector(
    const std::shared_ptr<DeviceContext<GraphicsBackend::Vulkan>>& deviceContext,
     const DescriptorSetLayoutHandle<GraphicsBackend::Vulkan>* layoutHandles,
    uint32_t layoutHandleCount)
: DescriptorSetVector<GraphicsBackend::Vulkan>(
    deviceContext,
    allocateDescriptorSets(
        deviceContext->getDevice(),
        deviceContext->getDescriptorPool(),
        layoutHandles,
        layoutHandleCount))
{
}

template <>
DescriptorSetVector<GraphicsBackend::Vulkan>::~DescriptorSetVector()
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
