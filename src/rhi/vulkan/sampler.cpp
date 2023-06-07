#include "../sampler.h"

#include "utils.h"

template <>
SamplerVector<Vk>::SamplerVector(
	const std::shared_ptr<Device<Vk>>& device,
	std::vector<SamplerHandle<Vk>>&& samplers)
	: DeviceObject(
		  device,
		  {"_Sampler"},
		  samplers.size(),
		  VK_OBJECT_TYPE_SAMPLER,
		  reinterpret_cast<uint64_t*>(samplers.data()))
	, mySamplers(std::forward<std::vector<SamplerHandle<Vk>>>(samplers))
{}

template <>
SamplerVector<Vk>::SamplerVector(
	const std::shared_ptr<Device<Vk>>& device,
	const std::vector<SamplerCreateInfo<Vk>>& createInfos)
	: SamplerVector<Vk>(
		device,
		createSamplers(
			*device,
			&device->getInstance()->getHostAllocationCallbacks(),
			createInfos))
{}

template <>
SamplerVector<Vk>::SamplerVector(SamplerVector&& other) noexcept
	: DeviceObject(std::forward<SamplerVector>(other))
	, mySamplers(std::exchange(other.mySamplers, {}))
{}

template <>
SamplerVector<Vk>::~SamplerVector()
{
	for (auto sampler : mySamplers)
		vkDestroySampler(
			*getDevice(),
			sampler,
			&getDevice()->getInstance()->getHostAllocationCallbacks());
}

template <>
SamplerVector<Vk>& SamplerVector<Vk>::operator=(SamplerVector&& other) noexcept
{
	DeviceObject::operator=(std::forward<SamplerVector>(other));
	mySamplers = std::exchange(other.mySamplers, {});
	return *this;
}

template <>
void SamplerVector<Vk>::swap(SamplerVector& rhs) noexcept
{
	DeviceObject::swap(rhs);
	std::swap(mySamplers, rhs.mySamplers);
}
