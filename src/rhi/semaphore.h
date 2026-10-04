#pragma once

#include <rhi/deviceobject.h>
#include <rhi/types.h>

#include <span>

namespace rhi
{

template <GraphicsApi G>
class Semaphore;

template <GraphicsApi G>
struct SemaphoreCreateDesc final : DeviceObjectCreateDesc<G>
{
	SemaphoreType type{};
	uint32_t flags = 0UL;
};

template <GraphicsApi G>
struct ObjectTraits<Semaphore<G>>
{
	using CreateDescType = SemaphoreCreateDesc<G>;
};

template <GraphicsApi G>
class Semaphore final : public DeviceObject<Semaphore<G>>
{
public:
	using SuperType = DeviceObject<Semaphore<G>>;
	using CreateDescType = ObjectTraits<Semaphore<G>>::CreateDescType;
	
	constexpr Semaphore() noexcept = default;
	explicit Semaphore(CreateDescType&& desc);
	Semaphore(Semaphore<G>&& other) noexcept;
	~Semaphore();

	[[nodiscard]] Semaphore& operator=(Semaphore&& other) noexcept;
	[[nodiscard]] operator bool() const noexcept { return mySemaphore != nullptr; }//NOLINT(google-explicit-constructor)
	[[nodiscard]] operator auto() const noexcept { return mySemaphore; }//NOLINT(google-explicit-constructor)

	void Swap(Semaphore& rhs) noexcept;
	friend void Swap(Semaphore& lhs, Semaphore& rhs) noexcept { lhs.Swap(rhs); }

	[[nodiscard]] uint64_t GetValue() const;

	[[maybe_unused]] bool Wait(uint64_t timelineValue = 0, uint64_t timeout = ~0ULL) const; //NOLINT(modernize-use-nodiscard)
	[[maybe_unused]] static bool Wait(
		DeviceHandle<G> device,
		std::span<const SemaphoreHandle<G>> semaphores,
		std::span<const uint64_t> semaphoreValues,
		uint64_t timeout = ~0ULL);

private:
	Semaphore(
		CreateDescType&& desc,
		SemaphoreHandle<G>&& handle);
	
	SemaphoreHandle<G> mySemaphore{};
};

} // namespace rhi
