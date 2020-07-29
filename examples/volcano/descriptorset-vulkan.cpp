#include "descriptorset.h"
#include "vk-utils.h"

namespace descriptorset
{

std::vector<DescriptorSetLayoutHandle<GraphicsBackend::Vulkan>> createDescriptorSetLayouts(
	DeviceHandle<GraphicsBackend::Vulkan> device,
	const DescriptorSetLayoutBindingsMap<GraphicsBackend::Vulkan>& bindings)
{
    std::vector<DescriptorSetLayoutHandle<GraphicsBackend::Vulkan>> outLayouts;
    outLayouts.reserve(bindings.size());
	for (auto& [space, layoutBindings] : bindings)
	    outLayouts.emplace_back(createDescriptorSetLayout(device, layoutBindings.data(), layoutBindings.size()));
	return outLayouts;
}

}

template <>
DescriptorSetLayoutVector<GraphicsBackend::Vulkan>::DescriptorSetLayoutVector(
    const std::shared_ptr<DeviceContext<GraphicsBackend::Vulkan>>& deviceContext,
    std::vector<DescriptorSetLayoutHandle<GraphicsBackend::Vulkan>>&& descriptorSetLayoutVector)
: DeviceResource<GraphicsBackend::Vulkan>(
    deviceContext,
    {"_DescriptorSetLayout"},
    descriptorSetLayoutVector.size(),
    VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT,
    reinterpret_cast<uint64_t*>(descriptorSetLayoutVector.data()))
, myDescriptorSetLayoutVector(std::move(descriptorSetLayoutVector))
{
}

template <>
DescriptorSetLayoutVector<GraphicsBackend::Vulkan>::DescriptorSetLayoutVector(
    const std::shared_ptr<DeviceContext<GraphicsBackend::Vulkan>>& deviceContext,
    const DescriptorSetLayoutBindingsMap<GraphicsBackend::Vulkan>& bindings)
: DescriptorSetLayoutVector<GraphicsBackend::Vulkan>(
    deviceContext,
    descriptorset::createDescriptorSetLayouts(deviceContext->getDevice(), bindings))
{
}

template <>
DescriptorSetLayoutVector<GraphicsBackend::Vulkan>::~DescriptorSetLayoutVector()
{
    if (myDescriptorSetLayoutVector.size())
        for (auto layout : myDescriptorSetLayoutVector)
            vkDestroyDescriptorSetLayout(getDeviceContext()->getDevice(), layout, nullptr);
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
DescriptorSetVector<GraphicsBackend::Vulkan>::DescriptorSetVector(
    const std::shared_ptr<DeviceContext<GraphicsBackend::Vulkan>>& deviceContext,
    const DescriptorSetLayoutVector<GraphicsBackend::Vulkan>& layouts)
: DescriptorSetVector<GraphicsBackend::Vulkan>(
    deviceContext,
    layouts.data(),
    layouts.size())
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
