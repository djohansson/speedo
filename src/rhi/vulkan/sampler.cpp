#include "../sampler.h"

#include "utils.h"

template <>
SamplerVector<kVk>::SamplerVector(
	const std::shared_ptr<Device<kVk>>& device,
	std::vector<SamplerHandle<kVk>>&& samplers)
	: DeviceObject(
		  device,
		  {"_Sampler"},
		  samplers.size(),
		  VK_OBJECT_TYPE_SAMPLER,
		  reinterpret_cast<uint64_t*>(samplers.data()))
	, mySamplers(std::forward<std::vector<SamplerHandle<kVk>>>(samplers))
{}

template <>
SamplerVector<kVk>::SamplerVector(
	const std::shared_ptr<Device<kVk>>& device,
	const std::vector<SamplerCreateInfo<kVk>>& createInfos)
	: SamplerVector<kVk>(
		device,
		[&device, &createInfos]
		{
			std::vector<SamplerHandle<kVk>> outSamplers;
			outSamplers.reserve(createInfos.size());

			for (const auto& createInfo : createInfos)
			{
				SamplerHandle<kVk> outSampler;
				VK_CHECK(vkCreateSampler(*device, &createInfo, &device->GetInstance()->GetHostAllocationCallbacks(), &outSampler));

				outSamplers.emplace_back(std::move(outSampler));
			}

			return outSamplers;
		}())
{}

template <>
SamplerVector<kVk>::SamplerVector(SamplerVector&& other) noexcept
	: DeviceObject(std::forward<SamplerVector>(other))
	, mySamplers(std::exchange(other.mySamplers, {}))
{}

template <>
SamplerVector<kVk>::~SamplerVector()
{
	for (auto* sampler : mySamplers)
		vkDestroySampler(
			*InternalGetDevice(),
			sampler,
			&InternalGetDevice()->GetInstance()->GetHostAllocationCallbacks());
}

template <>
SamplerVector<kVk>& SamplerVector<kVk>::operator=(SamplerVector&& other) noexcept
{
	DeviceObject::operator=(std::forward<SamplerVector>(other));
	mySamplers = std::exchange(other.mySamplers, {});
	return *this;
}

template <>
void SamplerVector<kVk>::Swap(SamplerVector& rhs) noexcept
{
	DeviceObject::Swap(rhs);
	std::swap(mySamplers, rhs.mySamplers);
}
