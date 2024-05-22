#pragma once

#include "assert.h"//NOLINT(modernize-deprecated-headers)
#include "upgradablesharedmutex.h"
#include "utils.h"

#include <algorithm>
#include <array>
#include <cstdint>

struct MemoryPoolHandle
{
	static constexpr uint32_t kInvalidIndex = ~0U;
	uint32_t value = kInvalidIndex;

	[[nodiscard]] constexpr explicit operator bool() const noexcept { return value != kInvalidIndex; }
	[[nodiscard]] constexpr auto operator<=>(const MemoryPoolHandle&) const noexcept = default;
};

template <typename T, uint32_t Capacity>
class MemoryPool final
{
public:
	constexpr MemoryPool() noexcept;
	MemoryPool(const MemoryPool&) = delete;
	MemoryPool(MemoryPool&&) noexcept = delete;

	MemoryPool& operator=(const MemoryPool&) = delete;
	MemoryPool& operator=(MemoryPool&&) noexcept = delete;

	[[nodiscard]] MemoryPoolHandle Allocate() noexcept;
	void Free(MemoryPoolHandle handle) noexcept;
	
	// never store the returned pointer, it may become invalid if the pool is resized
	[[nodiscard]] constexpr T* GetPointer(MemoryPoolHandle handle) noexcept;

	[[nodiscard]] constexpr MemoryPoolHandle GetHandle(const T* ptr) noexcept;

private:
	enum class State : uint8_t
	{
		kTaken = 0,
		kFree = 1
	};

	struct Entry
	{
		static constexpr auto kStateBits = 1;
		static constexpr auto kIndexBits = 31;
		State state : kStateBits;
		uint32_t index : kIndexBits;

		bool operator<(const Entry& other) const noexcept { return state < other.state; }
	};

	alignas(T) std::array<std::byte, Capacity * sizeof(T)> myPool;
	std::array<Entry, Capacity> myEntries;
	UpgradableSharedMutex myMutex;
	uint32_t myAvailable = 0;
};

#include "memorypool.inl"
