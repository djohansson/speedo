#pragma once

#include "device.h"
#include "types.h"

#include <memory>
#include <span>

template <GraphicsApi G>
struct FenceCreateDesc
{
	std::string_view name;
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
	[[nodiscard]] operator bool() const noexcept { return myFence != nullptr; }
	[[nodiscard]] operator auto() const noexcept { return myFence; }

	[[nodiscard]] const auto& GetHandle() const noexcept { return myFence; }

	void Swap(Fence& rhs) noexcept;
	friend void Swap(Fence& lhs, Fence& rhs) noexcept { lhs.Swap(rhs); }

	[[maybe_unused]] bool Wait(uint64_t timeout = ~0ULL) const;
	[[maybe_unused]] static bool Wait(DeviceHandle<G> device, std::span<const FenceHandle<G>> fences, bool waitAll = true, uint64_t timeout = ~0ULL);

private:
	Fence(const std::shared_ptr<Device<G>>& device, FenceHandle<G>&& fence, FenceCreateDesc<G>&& desc);
	
	FenceHandle<G> myFence{};
};
