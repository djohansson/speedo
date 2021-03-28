#pragma once

#include "device.h"
#include "types.h"

#include <memory>

template <GraphicsBackend B>
class SamplerVector : public DeviceObject<B>
{
public:

    constexpr SamplerVector() noexcept = default;
    SamplerVector(
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        const std::vector<SamplerCreateInfo<B>>& createInfos);
    SamplerVector(SamplerVector&& other) noexcept;
    ~SamplerVector();

    SamplerVector& operator=(SamplerVector&& other) noexcept;
    auto operator[](uint32_t index) const noexcept { return mySamplers[index]; };

    void swap(SamplerVector& rhs) noexcept;
    friend void swap(SamplerVector& lhs, SamplerVector& rhs) noexcept { lhs.swap(rhs); }

    auto size() const noexcept { return mySamplers.size(); }
    auto data() const noexcept { return mySamplers.data(); }

private:

    SamplerVector( // takes ownership of provided handle
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        std::vector<SamplerHandle<B>>&& samplers);

	 std::vector<SamplerHandle<B>> mySamplers = {};
};

#include "sampler-vulkan.inl"
