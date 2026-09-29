#pragma once

#include <rhi/deviceobject.h>
#include <rhi/types.h>

namespace rhi
{

template <GraphicsApi G>
class SamplerVector;

template <GraphicsApi G>
struct SamplerVectorCreateDesc final : DeviceObjectCreateDesc<G>
{
	std::vector<SamplerCreateInfo<G>> createInfos;
};

template <GraphicsApi G>
struct ObjectTraits<SamplerVector<G>>
{
	using CreateDescType = SamplerVectorCreateDesc<G>;
};

template <GraphicsApi G>
class SamplerVector final : public DeviceObject<SamplerVector<G>>
{
public:
	using SuperType = DeviceObject<SamplerVector<G>>;
	using CreateDescType = ObjectTraits<SamplerVector<G>>::CreateDescType;

	constexpr SamplerVector() noexcept = default;
	explicit SamplerVector(CreateDescType&& desc);
	SamplerVector(SamplerVector&& other) noexcept;
	~SamplerVector();

	[[maybe_unused]] SamplerVector& operator=(SamplerVector&& other) noexcept;
	[[nodiscard]] auto operator[](uint32_t index) const noexcept { return mySamplers[index]; };

	void Swap(SamplerVector& rhs) noexcept;
	friend void Swap(SamplerVector& lhs, SamplerVector& rhs) noexcept { lhs.Swap(rhs); }

	[[nodiscard]] auto Size() const noexcept { return mySamplers.size(); }
	[[nodiscard]] auto Data() const noexcept { return mySamplers.data(); }

private:
	explicit SamplerVector( // takes ownership of provided handles
		CreateDescType&& desc,
		std::vector<SamplerHandle<G>>&& samplers);

	std::vector<SamplerHandle<G>> mySamplers;
};

} // namespace rhi
