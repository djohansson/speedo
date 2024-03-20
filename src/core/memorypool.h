#pragma once

#include "upgradablesharedmutex.h"
#include "utils.h"

#include <algorithm>
#include <cassert>

#include <array>
#include <cstdint>

struct MemoryPoolHandle
{
	static constexpr uint32_t InvalidIndex = ~0u;
	uint32_t value = InvalidIndex;

	bool operator!() const { return value == InvalidIndex; }
	auto operator<=>(const MemoryPoolHandle&) const = default;
};

template <typename T, uint32_t Capacity>
class MemoryPool : public Noncopyable, public Nonmovable
{
public:
	constexpr MemoryPool() noexcept;

	MemoryPoolHandle allocate() noexcept;
	void free(MemoryPoolHandle handle) noexcept;

	T* getPointer(MemoryPoolHandle handle) noexcept { return reinterpret_cast<T*>(&myPool[handle.value * sizeof(T)]); }

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

		bool operator<(const Entry& other) const { return state < other.state; }
	};

	alignas(T) std::array<std::byte, Capacity * sizeof(T)> myPool;
	std::array<Entry, Capacity> myEntries;
	UpgradableSharedMutex<> myMutex;
	uint32_t myAvailable = 0;
};

#include "memorypool.inl"
