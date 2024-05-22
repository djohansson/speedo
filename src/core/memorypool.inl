#include <shared_mutex>

template <typename T, uint32_t Capacity>
constexpr MemoryPool<T, Capacity>::MemoryPool() noexcept
: myAvailable(Capacity)
{
	static_assert(Capacity > 0);
	static_assert(Capacity < (1 << Entry::kIndexBits));

	for (auto& entry : myEntries)
		entry = {State::kFree, static_cast<uint32_t>(&entry - myEntries.data())};

	std::make_heap(myEntries.begin(), myEntries.end());
}

template <typename T, uint32_t Capacity>
MemoryPoolHandle MemoryPool<T, Capacity>::Allocate() noexcept
{
	std::unique_lock lock(myMutex);

	ASSERT(myAvailable > 0);
	ASSERT(myAvailable <= Capacity);

	if (myAvailable == 0 || myAvailable > Capacity)
		return MemoryPoolHandle{};

	MemoryPoolHandle handle{myEntries[0].index};

	std::pop_heap(myEntries.begin(), myEntries.end());

	--myAvailable;

	myEntries[Capacity - 1] = {State::kTaken};

	ASSERT(std::is_heap(myEntries.begin(), myEntries.end()));

	return handle;
}

template <typename T, uint32_t Capacity>
void MemoryPool<T, Capacity>::Free(MemoryPoolHandle handle) noexcept
{
	std::unique_lock lock(myMutex);

	ASSERT(!!handle);
	ASSERT(myAvailable < Capacity);

	if (!handle || myAvailable >= Capacity)
		return;

	myEntries[Capacity - 1] = {State::kFree, handle.value};

	myAvailable++;

	std::push_heap(myEntries.begin(), myEntries.end());

	ASSERT(std::is_heap(myEntries.begin(), myEntries.end() - 1));
}

template <typename T, uint32_t Capacity>
constexpr T* MemoryPool<T, Capacity>::GetPointer(MemoryPoolHandle handle) noexcept
{
	ASSERT(!!handle);

	if (!handle)
		return nullptr;

	return reinterpret_cast<T*>(&myPool[handle.value * sizeof(T)]);
}

template <typename T, uint32_t Capacity>
constexpr MemoryPoolHandle MemoryPool<T, Capacity>::GetHandle(const T* ptr) noexcept
{
	ASSERT(ptr != nullptr);
	
	if (!ptr)
		return MemoryPoolHandle{};

	return MemoryPoolHandle{static_cast<uint32_t>((reinterpret_cast<const std::byte*>(ptr) - myPool.data()) / sizeof(T))};
}
