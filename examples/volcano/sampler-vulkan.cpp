#include "sampler.h"
#include "vk-utils.h"

template <>
Sampler<Vk>::Sampler(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
     SamplerHandle<Vk>&& sampler)
: DeviceResource<Vk>(
    deviceContext,
    {"_Sampler"},
    1,
    VK_OBJECT_TYPE_SAMPLER,
    reinterpret_cast<uint64_t*>(&sampler))
, mySampler(std::move(sampler))
{
}

template <>
Sampler<Vk>::Sampler(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    const SamplerCreateInfo<Vk>& createInfo)
: Sampler<Vk>(
    deviceContext,
    createSampler(deviceContext->getDevice(), createInfo))
{
}

template <>
Sampler<Vk>::~Sampler()
{
    if (mySampler)
        vkDestroySampler(getDeviceContext()->getDevice(), mySampler, nullptr);
}
