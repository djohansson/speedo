#pragma once

#include <core/std_extra.h>
#include <core/utils.h>

#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <mutex>
#include <vector>

namespace core
{

// a pool of T in chunks of ChunkSize, made as it needs them (up to MaxChunks, a hard failure past them, see CLAUDE.md,
// "Capacity limits") and kept until the pool is destroyed: an object never moves, so GetPointer takes no lock. a handle is
// an object's index and its slot's generation, which freeing it advances, so a handle used after its object was freed
// (and its slot perhaps taken again) traps rather than reaching another object. it was a fixed array of N objects, 16384
// tasks, which nested loops on 32 threads ran out of (see TaskExecutor::ForEach).
//
// the free slots are in shards, a thread's own (64 of them, each thread given the next in turn, so up to 64 threads never
// share one), each a stack of at most kShardCapacity under its own mutex, refilled from and drained to the pool's free list
// kBatch at a time: with one free list under one mutex, and the counts shared atomics, an allocation and a free took 15 ns
// on one thread and 12 us on each of 32 (taskbench's alloc: all of them together 4% of one alone). the shards are the
// pool's, not thread_local, which would outlive a pool its threads outlive. a shard's count is what was allocated through
// it less what was freed through it (a slot is often freed on another thread), Size their sum; Peak is sampled as shards
// refill, so it may miss a peak by up to a batch a shard
template <typename T, std::size_t ChunkSize, std::size_t MaxChunks>
class MemoryPool final
{
	static_assert(std::has_single_bit(ChunkSize));
	static constexpr std::size_t kIndexBits = 20;
	static constexpr std::size_t kGenerationBits = 12;
	static_assert(ChunkSize * MaxChunks < (std::size_t{1} << kIndexBits) - 1, "the invalid handle's index must stay unused");
	static constexpr std::size_t kShardCount = 64;
	static constexpr std::size_t kBatch = 32;
	static constexpr std::size_t kShardCapacity = 2 * kBatch;
	static_assert(ChunkSize >= kBatch);

public:
	// a 32 bit handle: MinSizeIndex's type holds its N
	using Handle = MinSizeIndex<(std::size_t{1} << (kIndexBits + kGenerationBits - 1)) - 1>;

	MemoryPool() noexcept = default;
	MemoryPool(const MemoryPool&) = delete;
	MemoryPool(MemoryPool&&) noexcept = delete;
	~MemoryPool() noexcept;

	MemoryPool& operator=(const MemoryPool&) = delete;
	MemoryPool& operator=(MemoryPool&&) noexcept = delete;

	[[nodiscard]] Handle Allocate() noexcept;
	void Free(Handle handle) noexcept;

	// the object's storage, which stays where it is until the pool is destroyed
	[[nodiscard]] T* GetPointer(Handle handle) const noexcept;

	[[nodiscard]] std::size_t Size() const noexcept;
	[[nodiscard]] std::size_t Peak() const noexcept { return myPeak.load(std::memory_order_relaxed); } // most allocated at once
	[[nodiscard]] std::size_t Capacity() const noexcept { return myChunkCount.load(std::memory_order_relaxed) * ChunkSize; }
	[[nodiscard]] static constexpr std::size_t MaxCapacity() noexcept { return ChunkSize * MaxChunks; }

private:
	struct Chunk
	{
		alignas(T) std::array<std::byte, ChunkSize * sizeof(T)> storage;
		std::array<std::atomic_uint16_t, ChunkSize> generations{};
	};

	struct alignas(std_extra::hardware_destructive_interference_size) Shard
	{
		std::mutex mutex;
		std::vector<std::uint32_t> free; // indices, the next to take last, at most kShardCapacity
		std::atomic_int64_t count = 0; // allocated through it less freed through it, written under its mutex
	};

	static constexpr std::uint32_t kIndexMask = (std::uint32_t{1} << kIndexBits) - 1;
	static constexpr std::uint32_t kGenerationMask = (std::uint32_t{1} << kGenerationBits) - 1;

	// the calling thread's shard (the threads given one each in turn)
	[[nodiscard]] Shard& InternalShard() noexcept;
	// a batch from the pool's free list into the shard, which is empty and locked: grows the pool if it must
	void InternalRefill(Shard& shard) noexcept;
	// the shard's oldest batch to the pool's free list: the shard is full and locked
	void InternalDrain(Shard& shard) noexcept;
	void InternalGrow() noexcept;

	std::array<std::atomic<Chunk*>, MaxChunks> myChunks{};
	std::atomic_size_t myChunkCount = 0;
	std::array<Shard, kShardCount> myShards;
	std::vector<std::uint32_t> myFree; // indices, the next to take last
	std::mutex myMutex; // guards myFree and growing, taken after a shard's mutex, never before: GetPointer needs no lock
	std::atomic_size_t myPeak = 0;
};

} // namespace core

#include "memorypool.inl"
