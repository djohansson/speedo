#pragma once

#include <rhi/deviceobject.h>
#include <rhi/types.h>

#include <span>

namespace rhi
{

template <GraphicsApi G>
class Fence;

template <GraphicsApi G>
struct FenceCreateDesc final : DeviceObjectCreateDesc<G>
{
	uint32_t flags = 0;
};

template <GraphicsApi G>
struct ObjectTraits<Fence<G>>
{
	using CreateDescType = FenceCreateDesc<G>;
};

template <GraphicsApi G>
class Fence final : public DeviceObject<Fence<G>>
{
public:
	using SuperType = DeviceObject<Fence<G>>;
	using CreateDescType = ObjectTraits<Fence<G>>::CreateDescType;

	constexpr Fence() noexcept = default;
	explicit Fence(CreateDescType&& desc);
	Fence(Fence<G>&& other) noexcept;
	~Fence();

	[[maybe_unused]] Fence& operator=(Fence&& other) noexcept;
	[[nodiscard]] operator bool() const noexcept { return myFence != nullptr; } //NOLINT(google-explicit-constructor)
	[[nodiscard]] operator auto() const noexcept { return myFence; } //NOLINT(google-explicit-constructor)

	[[nodiscard]] const auto& GetHandle() const noexcept { return myFence; }

	void Swap(Fence& rhs) noexcept;
	friend void Swap(Fence& lhs, Fence& rhs) noexcept { lhs.Swap(rhs); }

	[[maybe_unused]] bool Wait(uint64_t timeout = ~0ULL) const; // NOLINT(modernize-use-nodiscard)
	[[maybe_unused]] static bool Wait(DeviceHandle<G> device, std::span<const FenceHandle<G>> fences, bool waitAll = true, uint64_t timeout = ~0ULL);

private:
	Fence(CreateDescType&& desc, FenceHandle<G>&& fence);
	
	FenceHandle<G> myFence{};
};

} // namespace rhi
