#pragma once

#include "device.h"
#include "types.h"

#include <memory>
#include <span>

template <GraphicsApi G>
struct FenceCreateDesc
{
	uint32_t flags = 0;
};

template <GraphicsApi G>
class Fence : public DeviceObject<G>
{
public:
	constexpr Fence() noexcept = default;
	Fence(const std::shared_ptr<Device<G>>& device, FenceCreateDesc<G>&& desc);
	Fence(Fence<G>&& other) noexcept;
	~Fence();

	[[maybe_unused]] Fence& operator=(Fence&& other) noexcept;
	operator auto() const noexcept { return myFence; }

	void Swap(Fence& rhs) noexcept;
	friend void Swap(Fence& lhs, Fence& rhs) noexcept { lhs.Swap(rhs); }

	void Wait(bool waitAll = true, uint64_t timeout = ~0ull) const;
	static void Wait(const std::shared_ptr<Device<G>>& device, std::span<FenceHandle<G>> fences, bool waitAll = true, uint64_t timeout = ~0ull);

private:
	Fence(const std::shared_ptr<Device<G>>& device, FenceHandle<G>&& fence);
	
	FenceHandle<G> myFence{};
};
