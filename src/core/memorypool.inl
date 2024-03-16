

template <typename T, uint32_t Capacity>
MemoryPool<T, Capacity>::MemoryPool() 
: myAvailable(Capacity)
{
	static_assert(Capacity > 0);
	static_assert(Capacity < (1 << 31));

	for (auto& entry : myEntries)
		entry = {State::Free, static_cast<uint32_t>(&entry - myEntries.data())};

	std::make_heap(myEntries.begin(), myEntries.end());
}

template <typename T, uint32_t Capacity>
T* MemoryPool<T, Capacity>::allocate()
{
	assert(myAvailable > 0);
	assert(myAvailable <= Capacity);

	T* retval = myPool.data() + myEntries[0].offset;

	std::pop_heap(myEntries.begin(), myEntries.end());

	--myAvailable;

	myEntries[Capacity - 1] = {State::Taken};

	assert(std::is_heap(myEntries.begin(), myEntries.end()));

	return retval;
}

template <typename T, uint32_t Capacity>
void MemoryPool<T, Capacity>::free(T* ptr)
{
	assert(ptr);
	assert(myAvailable < Capacity);

	if (!ptr || myAvailable >= Capacity)
		return;

	myEntries[Capacity - 1] = {State::Free, static_cast<uint32_t>(ptr - myPool.data())};

	myAvailable++;

	std::push_heap(myEntries.begin(), myEntries.end());

	assert(std::is_heap(myEntries.begin(), myEntries.end() - 1));
}
