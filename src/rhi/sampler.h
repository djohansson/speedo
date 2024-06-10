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
	~SamplerVector() override;

	SamplerVector& operator=(SamplerVector&& other) noexcept;
	[[nodiscard]] auto operator[](uint32_t index) const noexcept { return mySamplers[index]; };

	void Swap(SamplerVector& rhs) noexcept;
	friend void Swap(SamplerVector& lhs, SamplerVector& rhs) noexcept { lhs.Swap(rhs); }

	[[nodiscard]] auto Size() const noexcept { return mySamplers.size(); }
	[[nodiscard]] auto Data() const noexcept { return mySamplers.data(); }

private:
	SamplerVector( // takes ownership of provided handle
		const std::shared_ptr<Device<G>>& device,
		std::vector<SamplerHandle<G>>&& samplers);

	std::vector<SamplerHandle<G>> mySamplers;
};
