#include <core/assert.h>//NOLINT(modernize-deprecated-headers)

#include <algorithm>
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
typename MemoryPool<T, ChunkSize, MaxChunks>::Shard& MemoryPool<T, ChunkSize, MaxChunks>::InternalShard() noexcept
{
	static std::atomic_uint32_t gNextShard = 0;
	static thread_local std::uint32_t tShard = gNextShard.fetch_add(1, std::memory_order_relaxed) % kShardCount;

	return myShards[tShard];
}

template <typename T, std::size_t ChunkSize, std::size_t MaxChunks>
void MemoryPool<T, ChunkSize, MaxChunks>::InternalRefill(Shard& shard) noexcept
{
	std::unique_lock lock(myMutex);

	if (myFree.size() < kBatch)
		InternalGrow();

	if (shard.free.capacity() < kShardCapacity + 1)
		shard.free.reserve(kShardCapacity + 1); // once: a shard's stack never allocates again
	shard.free.insert(shard.free.end(), myFree.end() - kBatch, myFree.end());
	myFree.resize(myFree.size() - kBatch);

	lock.unlock();

	// the most allocated at once, sampled here (see the class's comment)
	if (auto size = Size(); size > myPeak.load(std::memory_order_relaxed))
		myPeak.store(size, std::memory_order_relaxed);
}

template <typename T, std::size_t ChunkSize, std::size_t MaxChunks>
void MemoryPool<T, ChunkSize, MaxChunks>::InternalDrain(Shard& shard) noexcept
{
	// the oldest, at the bottom: the shard keeps the slots freed last, still in cache
	std::unique_lock lock(myMutex);
	myFree.insert(myFree.end(), shard.free.begin(), shard.free.begin() + kBatch);
	lock.unlock();

	shard.free.erase(shard.free.begin(), shard.free.begin() + kBatch);
}

template <typename T, std::size_t ChunkSize, std::size_t MaxChunks>
MemoryPool<T, ChunkSize, MaxChunks>::Handle MemoryPool<T, ChunkSize, MaxChunks>::Allocate() noexcept
{
	auto& shard = InternalShard();
	std::uint32_t index;
	{
		std::unique_lock lock(shard.mutex);

		if (shard.free.empty())
			InternalRefill(shard);

		index = shard.free.back();
		shard.free.pop_back();
		shard.count.store(shard.count.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
	}

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

	auto& shard = InternalShard();
	std::unique_lock lock(shard.mutex);

	shard.free.push_back(index);
	shard.count.store(shard.count.load(std::memory_order_relaxed) - 1, std::memory_order_relaxed);

	if (shard.free.size() > kShardCapacity)
		InternalDrain(shard);
}

template <typename T, std::size_t ChunkSize, std::size_t MaxChunks>
std::size_t MemoryPool<T, ChunkSize, MaxChunks>::Size() const noexcept
{
	std::int64_t size = 0;
	for (const auto& shard : myShards)
		size += shard.count.load(std::memory_order_relaxed);

	return static_cast<std::size_t>(std::max<std::int64_t>(size, 0)); // read while counts move: momentarily off
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
