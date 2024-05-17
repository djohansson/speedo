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
	Semaphore(const std::shared_ptr<Device<G>>& device, SemaphoreCreateDesc<G>&& desc);
	Semaphore(Semaphore<G>&& other) noexcept;
	~Semaphore();

	Semaphore& operator=(Semaphore&& other) noexcept;
	operator auto() const noexcept { return mySemaphore; }//NOLINT(google-explicit-constructor)

	void Swap(Semaphore& rhs) noexcept;
	friend void Swap(Semaphore& lhs, Semaphore& rhs) noexcept { lhs.Swap(rhs); }

	[[nodiscard]] uint64_t GetValue() const;

	void Wait(uint64_t timelineValue, uint64_t timeout = ~0ULL) const;
	static void Wait(
		const std::shared_ptr<Device<G>>& device,
		std::span<SemaphoreHandle<G>> semaphores,
		std::span<uint64_t> timelineValues,
		uint64_t timeout = ~0ULL);

private:
	Semaphore(const std::shared_ptr<Device<G>>& device, SemaphoreHandle<G>&& Semaphore);
	
	SemaphoreHandle<G> mySemaphore{};
};
