#pragma once

#include <rhi/types.h>

#include <cstdint>
#include <utility>

namespace rhi
{

// what a resource needs of the memory it is placed in (see Image::GetMemoryRequirements, Buffer::GetMemoryRequirements):
// its size, the alignment of its offset, and the memory types it can live in (a bit per type)
struct MemoryRequirements
{
	uint64_t size = 0;
	uint64_t alignment = 1;
	uint32_t memoryTypeBits = 0;
};

// a block of device memory that images and buffers can be placed in, at offsets (see the placing constructors of Image
// and Buffer), several of them over the same bytes if they are never used at the same time (aliasing: the first use of
// each must discard what the previous one left, see Image::Discard). the resources placed in it must be destroyed
// before it.
template <GraphicsApi G>
class MemoryBlock final
{
public:
	constexpr MemoryBlock() noexcept = default;
	// allocates requirements.size bytes of one of requirements.memoryTypeBits' types with flags, from device
	MemoryBlock(AllocatorHandle<G> allocator, const MemoryRequirements& requirements, MemoryProperty flags, const char* name);
	MemoryBlock(const MemoryBlock&) = delete;
	MemoryBlock(MemoryBlock&& other) noexcept { Swap(other); }
	~MemoryBlock();

	MemoryBlock& operator=(const MemoryBlock&) = delete;
	MemoryBlock& operator=(MemoryBlock&& other) noexcept
	{
		Swap(other);
		return *this;
	}

	void Swap(MemoryBlock& other) noexcept
	{
		std::swap(myAllocator, other.myAllocator);
		std::swap(myAllocation, other.myAllocation);
		std::swap(mySize, other.mySize);
	}

	[[nodiscard]] AllocatorHandle<G> GetAllocator() const noexcept { return myAllocator; }
	[[nodiscard]] AllocationHandle<G> GetAllocation() const noexcept { return myAllocation; }
	[[nodiscard]] uint64_t GetSize() const noexcept { return mySize; }
	[[nodiscard]] explicit operator bool() const noexcept { return myAllocation != AllocationHandle<G>{}; }

private:
	AllocatorHandle<G> myAllocator{};
	AllocationHandle<G> myAllocation{};
	uint64_t mySize = 0;
};

} // namespace rhi
