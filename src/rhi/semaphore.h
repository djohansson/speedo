#pragma once

#include "device.h"
#include "types.h"

#include <memory>
#include <span>

template <GraphicsApi G>
struct SemaphoreCreateDesc
{
	SemaphoreType<G> type{};
	uint32_t flags = 0UL;
};

template <GraphicsApi G>
class Semaphore : public DeviceObject<G>
{
public:
	constexpr Semaphore() noexcept = default;
	Semaphore(
		const std::shared_ptr<Device<G>>& device,
		SemaphoreCreateDesc<G>&& desc);
	Semaphore(Semaphore<G>&& other) noexcept;
	~Semaphore();

	[[nodiscard]] Semaphore& operator=(Semaphore&& other) noexcept;
	[[nodiscard]] operator auto() const noexcept { return mySemaphore; }//NOLINT(google-explicit-constructor)

	void Swap(Semaphore& rhs) noexcept;
	friend void Swap(Semaphore& lhs, Semaphore& rhs) noexcept { lhs.Swap(rhs); }

	[[nodiscard]] uint64_t GetValue() const;
	[[nodiscard]] const auto& GetDesc() const noexcept { return myDesc; }

	[[maybe_unused]] bool Wait(uint64_t timelineValue = 0, uint64_t timeout = ~0ULL) const;

private:
	Semaphore(
		const std::shared_ptr<Device<G>>& device,
		SemaphoreHandle<G>&& handle,
		SemaphoreCreateDesc<G>&& desc);
	
	SemaphoreHandle<G> mySemaphore{};
	SemaphoreCreateDesc<G> myDesc{};
};
