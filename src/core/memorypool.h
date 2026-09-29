#pragma once

#include <core/std_extra.h>
#include <core/upgradablesharedmutex.h>
#include <core/utils.h>

#include <array>
#include <atomic>

namespace core
{

template <typename T, std::size_t N>
class MemoryPool final
{
	using Handle = MinSizeIndex<N>;

public:
	constexpr MemoryPool() noexcept;
	MemoryPool(const MemoryPool&) = delete;
	MemoryPool(MemoryPool&&) noexcept = delete;

	MemoryPool& operator=(const MemoryPool&) = delete;
	MemoryPool& operator=(MemoryPool&&) noexcept = delete;

	[[nodiscard]] Handle Allocate() noexcept;
	void Free(Handle handle) noexcept;
	
	// never store the returned pointer, it may become invalid if the pool is resized
	[[nodiscard]] constexpr T* GetPointer(Handle handle) noexcept;
	[[nodiscard]] constexpr Handle GetHandle(const T* ptr) noexcept;

	[[nodiscard]] constexpr auto Size() const noexcept { return Capacity() - myAvailable; }
	[[nodiscard]] static consteval auto Capacity() noexcept { return N; }

private:
	struct Entry
	{
		static constexpr std::size_t kMaxIndex{N - 1};
		std_extra::min_unsigned_t<kMaxIndex> index;

		// we want lower indexes to be at the top of the heap
		[[nodiscard]] bool operator<(const Entry& other) const noexcept { return index >= other.index; }
	};

	alignas(T) std::array<std::byte, N * sizeof(T)> myPool;
	std::array<Entry, N> myEntries;
	UpgradableSharedMutex myMutex;
	std::atomic<std_extra::min_unsigned_t<N>> myAvailable{0};
};

} // namespace core

#include "memorypool.inl"
