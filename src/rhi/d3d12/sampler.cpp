// the D3D12 backend's samplers: each a descriptor in the device's cpu heap of samplers, copied into a set's sampler table
// where the set is written (pipelinedescriptors.cpp)
#include <rhi/d3d12/backend.h>

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

IMPLEMENT_OBJECT_GETINSTANCE(SamplerVector<kD3D12>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(SamplerVector<kD3D12>);

namespace sampler
{

static D3D12_TEXTURE_ADDRESS_MODE ToD3D12(AddressMode mode) noexcept
{
	switch (mode)
	{
	case AddressMode::kRepeat: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	case AddressMode::kMirroredRepeat: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
	case AddressMode::kClampToEdge: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	case AddressMode::kClampToBorder: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
	}
	return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
}

// the filters (anisotropic where above 1, which D3D12 takes for minification, magnification and mips alike)
static D3D12_FILTER Filter(const SamplerDesc& desc) noexcept
{
	if (desc.maxAnisotropy > 1.0F)
		return D3D12_FILTER_ANISOTROPIC;
	auto type = [](rhi::Filter filter) { return filter == rhi::Filter::kNearest ? D3D12_FILTER_TYPE_POINT : D3D12_FILTER_TYPE_LINEAR; };
	return D3D12_ENCODE_BASIC_FILTER(type(desc.minFilter), type(desc.magFilter), type(desc.mipmapFilter), D3D12_FILTER_REDUCTION_TYPE_STANDARD);
}

} // namespace sampler

template <>
SamplerVector<kD3D12>::SamplerVector(CreateDescType&& desc, std::vector<SamplerHandle<kD3D12>>&& samplers)
	: DeviceObject<SamplerVector<kD3D12>>(std::forward<CreateDescType>(desc))
	, mySamplers(std::forward<std::vector<SamplerHandle<kD3D12>>>(samplers))
{}

template <>
SamplerVector<kD3D12>::SamplerVector(CreateDescType&& desc)
	: SamplerVector(
		std::forward<CreateDescType>(desc),
		[&desc]
		{
			auto& allocator = d3d12::AllocatorObject::ForDevice(desc.device);
			std::vector<SamplerHandle<kD3D12>> samplers;
			samplers.reserve(desc.samplers.size());
			for (const auto& sampler : desc.samplers)
			{
				const D3D12_SAMPLER_DESC samplerDesc{
					.Filter = sampler::Filter(sampler),
					.AddressU = sampler::ToD3D12(sampler.addressModeU),
					.AddressV = sampler::ToD3D12(sampler.addressModeV),
					.AddressW = sampler::ToD3D12(sampler.addressModeW),
					.MipLODBias = sampler.mipLodBias,
					.MaxAnisotropy = std::clamp<UINT>(static_cast<UINT>(std::lround(sampler.maxAnisotropy)), 1, 16),
					.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER,
					.BorderColor = {0.0F, 0.0F, 0.0F, 0.0F},
					.MinLOD = sampler.minLod,
					.MaxLOD = sampler.maxLod,
				};
				auto* object = new d3d12::SamplerObject{.descriptor = allocator.samplers.Allocate(), .desc = samplerDesc};
				desc.device->CreateSampler(&samplerDesc, object->descriptor);
				samplers.push_back(object);
			}
			return samplers;
		}())
{}

template <>
SamplerVector<kD3D12>::~SamplerVector()
{
	if (mySamplers.empty())
		return;
	auto& allocator = d3d12::AllocatorObject::ForDevice(GetDesc().device);
	for (auto* sampler : mySamplers)
	{
		allocator.samplers.Free(sampler->descriptor);
		delete sampler;
	}
}

} // namespace rhi
