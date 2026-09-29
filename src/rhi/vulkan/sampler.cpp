#include <rhi/sampler.h>
#include <rhi/device.h>
#include <rhi/instance.h>
#include <rhi/rhiapplication.h>
#include <rhi/vulkan/utils.h>

namespace rhi
{

IMPLEMENT_OBJECT_GETINSTANCE(SamplerVector<kVk>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(SamplerVector<kVk>);

template <>
SamplerVector<kVk>::SamplerVector(
	CreateDescType&& desc,
	std::vector<SamplerHandle<kVk>>&& samplers)
	: DeviceObject<SamplerVector<kVk>>(std::forward<CreateDescType>(desc))
	, mySamplers(std::forward<std::vector<SamplerHandle<kVk>>>(samplers))
{}

template <>
SamplerVector<kVk>::SamplerVector(CreateDescType&& desc)
	: SamplerVector<kVk>(
		std::forward<CreateDescType>(desc),
		// read from desc, not GetDesc(): this runs before the delegated constructor has initialized the base
		[this, &desc]
		{
			std::vector<SamplerHandle<kVk>> outSamplers;
			outSamplers.reserve(desc.createInfos.size());

			for (const auto& createInfo : desc.createInfos)
			{
				SamplerHandle<kVk> outSampler;
				VK_CHECK(vkCreateSampler(
					desc.device,
					&createInfo,
					&GetInstance().GetHostAllocationCallbacks(),
					&outSampler));

				outSamplers.emplace_back(outSampler);
			}

			return outSamplers;
		}())
{}

template <>
void SamplerVector<kVk>::Swap(SamplerVector& rhs) noexcept
{
	DeviceObject<SamplerVector<kVk>>::Swap(rhs);
	std::swap(mySamplers, rhs.mySamplers);
}


template <>
SamplerVector<kVk>::SamplerVector(SamplerVector&& other) noexcept
{
	Swap(other);
}

template <>
SamplerVector<kVk>::~SamplerVector()
{
	for (auto* sampler : mySamplers)
		vkDestroySampler(
			GetDevice(),
			sampler,
			&GetInstance().GetHostAllocationCallbacks());
}

template <>
SamplerVector<kVk>& SamplerVector<kVk>::operator=(SamplerVector&& other) noexcept
{
	Swap(other);
	return *this;
}

} // namespace rhi
