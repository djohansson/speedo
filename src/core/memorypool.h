#pragma once

#include "utils.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>

template <typename T, uint32_t Capacity>
class MemoryPool : public Noncopyable, public Nonmovable
{
public:
	MemoryPool();

	T* allocate();
	void free(T* ptr);

private:
	enum class State : uint32_t
	{
		Taken = 0,
		Free = 1
	};

	struct Entry
	{
		State state : 1;
		uint32_t offset : 31;

		bool operator<(const Entry& other) const { return state < other.state; }
	};

	alignas(T) std::array<T, Capacity> myPool;
	std::array<Entry, Capacity> myEntries;
	uint32_t myAvailable = 0;
};

#include "memorypool.inl"
