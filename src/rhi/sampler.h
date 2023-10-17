#pragma once

#include "device.h"
#include "types.h"

#include <memory>

template <GraphicsApi G>
class SamplerVector final : public DeviceObject<G>
{
public:
	constexpr SamplerVector() noexcept = default;
	SamplerVector(
		const std::shared_ptr<Device<G>>& device,
		const std::vector<SamplerCreateInfo<G>>& createInfos);
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
		const std::shared_ptr<Device<G>>& device,
		std::vector<SamplerHandle<G>>&& samplers);

	std::vector<SamplerHandle<G>> mySamplers;
};
