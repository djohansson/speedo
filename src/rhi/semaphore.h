#pragma once

#include "device.h"
#include "types.h"

#include <memory>
#include <span>

template <GraphicsApi G>
struct SemaphoreCreateDesc
{
	SemaphoreType<G> type{};
	uint32_t flags = 0ul;
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
	operator auto() const noexcept { return mySemaphore; }

	void swap(Semaphore& rhs) noexcept;
	friend void swap(Semaphore& lhs, Semaphore& rhs) noexcept { lhs.swap(rhs); }

	uint64_t getValue() const;

	void wait(uint64_t timelineValue, uint64_t timeout = ~0ull) const;
	static void wait(const std::shared_ptr<Device<G>>& device, std::span<SemaphoreHandle<G>> semaphores, std::span<uint64_t> timelineValues, uint64_t timeout = ~0ull);

private:
	Semaphore(const std::shared_ptr<Device<G>>& device, SemaphoreHandle<G>&& Semaphore);
	
	SemaphoreHandle<G> mySemaphore{};
};
