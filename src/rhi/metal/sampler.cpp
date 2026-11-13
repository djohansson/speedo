// the Metal backend's samplers: sampler states made for argument buffers (a descriptor set holds their resource ids)
#include <rhi/metal/backend.h>

#include <rhi/device.h>
#include <rhi/rhi.h>
#include <rhi/sampler.h>

#include <core/assert.h>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace rhi
{

IMPLEMENT_OBJECT_GETINSTANCE(SamplerVector<kMetal>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(SamplerVector<kMetal>);

namespace sampler
{

static MTL::SamplerMinMagFilter ToMetal(Filter filter) noexcept
{
	return filter == Filter::kNearest ? MTL::SamplerMinMagFilterNearest : MTL::SamplerMinMagFilterLinear;
}

static MTL::SamplerMipFilter ToMetalMip(Filter filter) noexcept
{
	return filter == Filter::kNearest ? MTL::SamplerMipFilterNearest : MTL::SamplerMipFilterLinear;
}

static MTL::SamplerAddressMode ToMetal(AddressMode mode) noexcept
{
	switch (mode)
	{
	case AddressMode::kRepeat: return MTL::SamplerAddressModeRepeat;
	case AddressMode::kMirroredRepeat: return MTL::SamplerAddressModeMirrorRepeat;
	case AddressMode::kClampToEdge: return MTL::SamplerAddressModeClampToEdge;
	case AddressMode::kClampToBorder: return MTL::SamplerAddressModeClampToBorderColor;
	}
	return MTL::SamplerAddressModeRepeat;
}

} // namespace sampler

template <>
SamplerVector<kMetal>::SamplerVector(CreateDescType&& desc, std::vector<SamplerHandle<kMetal>>&& samplers)
	: DeviceObject<SamplerVector<kMetal>>(std::forward<CreateDescType>(desc))
	, mySamplers(std::forward<std::vector<SamplerHandle<kMetal>>>(samplers))
{}

template <>
SamplerVector<kMetal>::SamplerVector(CreateDescType&& desc)
	: SamplerVector(
		std::forward<CreateDescType>(desc),
		[&desc]
		{
			std::vector<SamplerHandle<kMetal>> samplers;
			samplers.reserve(desc.samplers.size());
			for (const auto& sampler : desc.samplers)
			{
				auto* descriptor = MTL::SamplerDescriptor::alloc()->init();
				descriptor->setMagFilter(sampler::ToMetal(sampler.magFilter));
				descriptor->setMinFilter(sampler::ToMetal(sampler.minFilter));
				descriptor->setMipFilter(sampler::ToMetalMip(sampler.mipmapFilter));
				descriptor->setSAddressMode(sampler::ToMetal(sampler.addressModeU));
				descriptor->setTAddressMode(sampler::ToMetal(sampler.addressModeV));
				descriptor->setRAddressMode(sampler::ToMetal(sampler.addressModeW));
				descriptor->setLodBias(sampler.mipLodBias);
				descriptor->setLodMinClamp(sampler.minLod);
				descriptor->setLodMaxClamp(sampler.maxLod);
				descriptor->setMaxAnisotropy(std::max<NS::UInteger>(1, static_cast<NS::UInteger>(std::lround(sampler.maxAnisotropy))));
				descriptor->setSupportArgumentBuffers(true);
				descriptor->setLabel(NS::String::string(desc.name.c_str(), NS::UTF8StringEncoding));
				MTL::SamplerState* state = desc.device->newSamplerState(descriptor);
				descriptor->release();
				ENSUREF(state != nullptr, "Failed to create a Metal sampler state");
				samplers.push_back(state);
			}
			return samplers;
		}())
{}

template <>
SamplerVector<kMetal>::~SamplerVector()
{
	for (auto* sampler : mySamplers)
		sampler->release();
}

} // namespace rhi
