#include "sampler.h"
#include "vk-utils.h"

template <>
SamplerVector<Vk>::SamplerVector(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    std::vector<SamplerHandle<Vk>>&& samplers)
: DeviceResource<Vk>(
    deviceContext,
    {"_Sampler"},
    samplers.size(),
    VK_OBJECT_TYPE_SAMPLER,
    reinterpret_cast<uint64_t*>(samplers.data()))
, mySamplers(std::move(samplers))
{
}

template <>
SamplerVector<Vk>::SamplerVector(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    const std::vector<SamplerCreateInfo<Vk>>& createInfos)
: SamplerVector<Vk>(
    deviceContext,
    createSamplers(deviceContext->getDevice(), createInfos))
{
}

template <>
SamplerVector<Vk>::SamplerVector(SamplerVector<Vk>&& other)
: DeviceResource<Vk>(std::move(other))
, mySamplers(std::exchange(other.mySamplers, {}))
{
}

template <>
SamplerVector<Vk>::~SamplerVector()
{
    for (auto sampler : mySamplers)
        vkDestroySampler(getDeviceContext()->getDevice(), sampler, nullptr);
}

template <>
SamplerVector<Vk>& SamplerVector<Vk>::operator=(SamplerVector<Vk>&& other)
{
	DeviceResource<Vk>::operator=(std::move(other));
	mySamplers = std::exchange(other.mySamplers, {});
	return *this;
}
