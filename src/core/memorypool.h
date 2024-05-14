#pragma once

#include "assert.h"//NOLINT(modernize-deprecated-headers)
#include "upgradablesharedmutex.h"
#include "utils.h"

#include <algorithm>
#include <array>
#include <cstdint>

struct MemoryPoolHandle
{
	static constexpr uint32_t InvalidIndex = ~0u;
	uint32_t value = InvalidIndex;

	constexpr bool operator!() const noexcept { return value == InvalidIndex; }
	constexpr auto operator<=>(const MemoryPoolHandle&) const noexcept = default;
};

template <typename T, uint32_t Capacity>
class MemoryPool : public Noncopyable, public Nonmovable
{
public:
	constexpr MemoryPool() noexcept;

	[[nodiscard]]
	MemoryPoolHandle Allocate() noexcept;
	void Free(MemoryPoolHandle handle) noexcept;
	
	// never store the returned pointer, it may become invalid if the pool is resized
	[[nodiscard]]
	constexpr T* GetPointer(MemoryPoolHandle handle) noexcept;

	[[nodiscard]]
	constexpr MemoryPoolHandle GetHandle(const T* ptr) noexcept;

private:
	enum class State : uint32_t
	{
		Taken = 0,
		Free = 1
	};

	struct Entry
	{
		State state : 1;
		uint32_t index : 31;

		bool operator<(const Entry& other) const noexcept { return state < other.state; }
	};

	alignas(T) std::array<std::byte, Capacity * sizeof(T)> myPool;
	std::array<Entry, Capacity> myEntries;
	UpgradableSharedMutex myMutex;
	uint32_t myAvailable = 0;
};

#include "memorypool.inl"
