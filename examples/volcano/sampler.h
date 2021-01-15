#pragma once

#include "device.h"
#include "types.h"

#include <memory>

template <GraphicsBackend B>
class SamplerVector : public DeviceResource<B>
{
public:

    SamplerVector(
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        const std::vector<SamplerCreateInfo<B>>& createInfos);
    SamplerVector(SamplerVector&& other);
    ~SamplerVector();

    SamplerVector& operator=(SamplerVector&& other);
    auto operator[](uint32_t index) const { return mySamplers[index]; };
    
    auto size() const { return mySamplers.size(); }
    auto data() const { return mySamplers.data(); }

private:

    SamplerVector( // takes ownership of provided handle
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        std::vector<SamplerHandle<B>>&& samplers);

	 std::vector<SamplerHandle<B>> mySamplers = {};
};

#include "sampler-vulkan.inl"
