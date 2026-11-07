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
// (and its slot perhaps taken again) traps rather than reaching another object. the free list is a stack: the slot freed
// last, still in cache, is taken first. it was a fixed array of N objects, 16384 tasks, which nested loops on 32 threads
// ran out of (see TaskExecutor::ForEach)
template <typename T, std::size_t ChunkSize, std::size_t MaxChunks>
class MemoryPool final
{
	static_assert(std::has_single_bit(ChunkSize));
	static constexpr std::size_t kIndexBits = 20;
	static constexpr std::size_t kGenerationBits = 12;
	static_assert(ChunkSize * MaxChunks < (std::size_t{1} << kIndexBits) - 1, "the invalid handle's index must stay unused");

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

	[[nodiscard]] std::size_t Size() const noexcept { return mySize.load(std::memory_order_relaxed); }
	[[nodiscard]] std::size_t Peak() const noexcept { return myPeak.load(std::memory_order_relaxed); } // most allocated at once
	[[nodiscard]] std::size_t Capacity() const noexcept { return myChunkCount.load(std::memory_order_relaxed) * ChunkSize; }
	[[nodiscard]] static constexpr std::size_t MaxCapacity() noexcept { return ChunkSize * MaxChunks; }

private:
	struct Chunk
	{
		alignas(T) std::array<std::byte, ChunkSize * sizeof(T)> storage;
		std::array<std::atomic_uint16_t, ChunkSize> generations{};
	};

	static constexpr std::uint32_t kIndexMask = (std::uint32_t{1} << kIndexBits) - 1;
	static constexpr std::uint32_t kGenerationMask = (std::uint32_t{1} << kGenerationBits) - 1;

	void InternalGrow() noexcept;

	std::array<std::atomic<Chunk*>, MaxChunks> myChunks{};
	std::atomic_size_t myChunkCount = 0;
	std::vector<std::uint32_t> myFree; // indices, the next to take last
	std::mutex myMutex; // guards myFree and growing: GetPointer needs no lock
	std::atomic_size_t mySize = 0;
	std::atomic_size_t myPeak = 0;
};

} // namespace core

#include "memorypool.inl"
