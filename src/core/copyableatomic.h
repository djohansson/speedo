#pragma once

#include <atomic>

template <typename T, typename AtomicT = std::atomic<T>>
class CopyableAtomic : public AtomicT
{
public:
	using value_t = T;
	using atomic_t = AtomicT;
	using atomic_t::store;

	constexpr CopyableAtomic() noexcept = default;
	constexpr explicit CopyableAtomic(value_t val) noexcept;
	CopyableAtomic(const CopyableAtomic& other) noexcept;

	[[maybe_unused]] CopyableAtomic& operator=(const CopyableAtomic& other) noexcept;
};

#include "copyableatomic.inl"
