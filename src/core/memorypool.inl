#include "assert.h"//NOLINT(modernize-deprecated-headers)

#include <shared_mutex>

template <typename T, std::size_t N>
constexpr MemoryPool<T, N>::MemoryPool() noexcept
{
	static_assert(Capacity() > 0);

	for (auto& entry : myEntries)
		entry.index = myAvailable++;

	ENSURE(myAvailable == Capacity());

	std::make_heap(myEntries.begin(), myEntries.end());
}

template <typename T, std::size_t N>
MemoryPool<T, N>::Handle MemoryPool<T, N>::Allocate() noexcept
{
	std::unique_lock lock(myMutex);

	ENSURE(myAvailable > 0);
	ENSURE(myAvailable <= Capacity());

	if (myAvailable == 0 || myAvailable > Capacity())
		return Handle{};

	Handle handle{myEntries[0].index};

	std::pop_heap(myEntries.begin(), myEntries.begin() + myAvailable);

	--myAvailable;

	ENSURE(std::is_heap(myEntries.begin(), myEntries.begin() + myAvailable));

	return handle;
}

template <typename T, std::size_t N>
void MemoryPool<T, N>::Free(Handle handle) noexcept
{
	std::unique_lock lock(myMutex);

	ENSURE(!!handle);
	ENSURE(myAvailable < Capacity());

	myEntries[myAvailable].index = handle.value;

	myAvailable++;

	std::push_heap(myEntries.begin(), myEntries.begin() + myAvailable);

	ENSURE(std::is_heap(myEntries.begin(), myEntries.begin() + myAvailable));
}

template <typename T, std::size_t N>
constexpr T* MemoryPool<T, N>::GetPointer(Handle handle) noexcept
{
	ENSURE(!!handle);

	return reinterpret_cast<T*>(&myPool[handle.value * sizeof(T)]);
}

template <typename T, std::size_t N>
constexpr MemoryPool<T, N>::Handle MemoryPool<T, N>::GetHandle(const T* ptr) noexcept
{
	ENSURE(ptr != nullptr);

	return Handle{static_cast<std_extra::min_unsigned_t<N>>(ptr - reinterpret_cast<const T*>(myPool.data()))};
}
