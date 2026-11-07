#include <core/assert.h>//NOLINT(modernize-deprecated-headers)

#include <new>

namespace core
{

template <typename T, std::size_t ChunkSize, std::size_t MaxChunks>
MemoryPool<T, ChunkSize, MaxChunks>::~MemoryPool() noexcept
{
	// the objects still allocated are the owners' to destroy: only their storage goes
	for (auto& chunk : myChunks)
		delete chunk.load(std::memory_order_relaxed);
}

template <typename T, std::size_t ChunkSize, std::size_t MaxChunks>
void MemoryPool<T, ChunkSize, MaxChunks>::InternalGrow() noexcept
{
	auto chunkIndex = myChunkCount.load(std::memory_order_relaxed);
	ENSUREF(chunkIndex < MaxChunks, "MemoryPool: all {} chunks of {} are in use ({} objects): a leak?", MaxChunks, ChunkSize,
		MaxCapacity());

	myChunks[chunkIndex].store(new Chunk, std::memory_order_release);
	myChunkCount.store(chunkIndex + 1, std::memory_order_relaxed);

	// the chunk's lowest index on top
	auto first = static_cast<std::uint32_t>(chunkIndex * ChunkSize);
	for (auto index = first + ChunkSize; index-- > first;)
		myFree.push_back(index);
}

template <typename T, std::size_t ChunkSize, std::size_t MaxChunks>
MemoryPool<T, ChunkSize, MaxChunks>::Handle MemoryPool<T, ChunkSize, MaxChunks>::Allocate() noexcept
{
	std::uint32_t index;
	{
		std::unique_lock lock(myMutex);

		if (myFree.empty())
			InternalGrow();

		index = myFree.back();
		myFree.pop_back();
	}

	if (auto size = mySize.fetch_add(1, std::memory_order_relaxed) + 1; size > myPeak.load(std::memory_order_relaxed))
		myPeak.store(size, std::memory_order_relaxed); // a peak another thread raises at once may be lost: statistics

	const auto& chunk = *myChunks[index / ChunkSize].load(std::memory_order_acquire);
	std::uint32_t generation = chunk.generations[index % ChunkSize].load(std::memory_order_relaxed);

	return Handle{(generation << kIndexBits) | index};
}

template <typename T, std::size_t ChunkSize, std::size_t MaxChunks>
void MemoryPool<T, ChunkSize, MaxChunks>::Free(Handle handle) noexcept
{
	ENSURE(!!handle);

	std::uint32_t index = handle.value & kIndexMask;
	auto& chunk = *myChunks[index / ChunkSize].load(std::memory_order_acquire);
	auto& generation = chunk.generations[index % ChunkSize];
	auto handleGeneration = handle.value >> kIndexBits;
	ENSUREF(generation.load(std::memory_order_relaxed) == handleGeneration, "MemoryPool: a handle freed twice");
	generation.store((handleGeneration + 1) & kGenerationMask, std::memory_order_relaxed);

	mySize.fetch_sub(1, std::memory_order_relaxed);

	std::unique_lock lock(myMutex);
	myFree.push_back(index);
}

template <typename T, std::size_t ChunkSize, std::size_t MaxChunks>
T* MemoryPool<T, ChunkSize, MaxChunks>::GetPointer(Handle handle) const noexcept
{
	// no lock: chunks never move, and the one a handle names was published before the handle was handed out
	ENSURE(!!handle);

	std::uint32_t index = handle.value & kIndexMask;
	auto* chunk = myChunks[index / ChunkSize].load(std::memory_order_acquire);
	ENSURE(chunk != nullptr);
	ENSUREF(chunk->generations[index % ChunkSize].load(std::memory_order_relaxed) == handle.value >> kIndexBits,
		"MemoryPool: a handle used after its object was freed");

	return std::launder(reinterpret_cast<T*>(chunk->storage.data() + ((index % ChunkSize) * sizeof(T))));
}

} // namespace core
