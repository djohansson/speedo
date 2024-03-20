#include <algorithm>
#include <cassert>
#include <shared_mutex>

template <typename T, uint32_t Capacity>
constexpr MemoryPool<T, Capacity>::MemoryPool() noexcept
: myAvailable(Capacity)
{
	static_assert(Capacity > 0);
	static_assert(Capacity < (1 << 31));

	for (auto& entry : myEntries)
		entry = {State::Free, static_cast<uint32_t>(&entry - myEntries.data())};

	std::make_heap(myEntries.begin(), myEntries.end());
}

template <typename T, uint32_t Capacity>
uint32_t MemoryPool<T, Capacity>::allocate() noexcept
{
	std::unique_lock lock(myMutex);

	assert(myAvailable > 0);
	assert(myAvailable <= Capacity);

	uint32_t retval = myEntries[0].index;

	std::pop_heap(myEntries.begin(), myEntries.end());

	--myAvailable;

	myEntries[Capacity - 1] = {State::Taken};

	assert(std::is_heap(myEntries.begin(), myEntries.end()));

	return retval;
}

template <typename T, uint32_t Capacity>
void MemoryPool<T, Capacity>::free(uint32_t index) noexcept
{
	std::unique_lock lock(myMutex);

	assert(myAvailable < Capacity);

	if (myAvailable >= Capacity)
		return;

	myEntries[Capacity - 1] = {State::Free, index};

	myAvailable++;

	std::push_heap(myEntries.begin(), myEntries.end());

	assert(std::is_heap(myEntries.begin(), myEntries.end() - 1));
}
