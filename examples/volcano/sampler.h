#pragma once

#include "device.h"
#include "types.h"

#include <memory>

template <GraphicsBackend B>
class Sampler : public DeviceResource<B>
{
public:

    Sampler(
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        const SamplerCreateInfo<B>& createInfo);
    Sampler( // takes ownership of provided handle
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        SamplerHandle<B>&& sampler);
    Sampler(Sampler&& other);
    ~Sampler();

    Sampler& operator=(Sampler&& other);
    operator auto() const { return mySampler; }

private:

	SamplerHandle<B> mySampler = {};
};

#include "sampler.inl"
