// TODO(djohansson): remove vertex.h/inl/cpp
#pragma once

#include <core/utils.h>

#include <stack>
#include <vector>

#include <xxhash.h>

// todo: delete this whole file - use gltf instead
class VertexAllocator
{
public:
	VertexAllocator() noexcept = default;

	void Lock()
	{
		ASSERT(!myIsLocked);

		myIsLocked = true;
	}

	void Unlock()
	{
		ASSERT(myIsLocked);

		myIsLocked = false;
	}

	[[nodiscard]] bool IsLocked() const { return myIsLocked; }
	[[nodiscard]] size_t Size() const { return myData.size() / Stride(); }
	[[nodiscard]] size_t SizeBytes() const { return myData.size(); }

	void Reserve(size_t size)
	{
		ASSERT(myIsLocked);

		myData.reserve(size * Stride());
	}

	void SetStride(size_t stride)
	{
		ASSERT(!myIsLocked);

		myStride = stride;
	}

	[[nodiscard]] size_t Stride() const { return myStride; }
	[[nodiscard]] const void* Data() const { return myData.data(); }
	[[nodiscard]] bool Empty() const { return myData.empty(); }

	// todo: handle alignment properly
	// todo: rewrite without vector
	// todo: handle pointer invalidation. std::deque? needs to be flattened before upload to gpu in that case.
	void* Allocate(size_t count = 1);

	// unless freeing the last item this is _very_ inefficient
	void Free(void* ptr, size_t count = 1);

	void Clear();

private:
	std::vector<char> myData;
	size_t myStride = 0;
	bool myIsLocked = false;
};

class ScopedVertexAllocation;

// todo: alignment for sse types, etc
class Vertex
{
	static constexpr size_t kDefaultSeed = 42;
public:
	Vertex() = delete;
	~Vertex() = delete;
	Vertex(const Vertex& other) = delete;
	Vertex& operator=(const Vertex& other) = delete;

	[[nodiscard]] bool operator==(const Vertex& other) const
	{
		return std::equal(
			this,
			reinterpret_cast<const Vertex*>(
				reinterpret_cast<const std::byte*>(this) + InternalAllocator().Stride()),
			&other);
	}

	[[nodiscard]] uint64_t Hash(size_t seed = kDefaultSeed) const
	{
		return XXH64(reinterpret_cast<const std::byte*>(this), InternalAllocator().Stride(), seed);
	}

	template <typename T>
	[[nodiscard]] T& DataAs(size_t offset = 0)
	{
		return *reinterpret_cast<T*>(reinterpret_cast<std::byte*>(this) + offset);
	}

	template <typename T>
	[[nodiscard]] const T& DataAs(size_t offset = 0) const
	{
		return *reinterpret_cast<const T*>(reinterpret_cast<const std::byte*>(this) + offset);
	}

	[[nodiscard]] static ScopedVertexAllocation* GetScope() { return gAllocationScope; }

private:
	friend ScopedVertexAllocation;

	[[nodiscard]] static VertexAllocator& InternalAllocator();

	static void InternalSetScope(ScopedVertexAllocation* scope) { gAllocationScope = scope; }

	static thread_local ScopedVertexAllocation* gAllocationScope;//NOLINT(readability-identifier-naming)
};

static_assert(sizeof(Vertex) == std::alignment_of_v<Vertex>);

class ScopedVertexAllocation final
{
public:
	explicit ScopedVertexAllocation(VertexAllocator& allocator);
	ScopedVertexAllocation(const ScopedVertexAllocation&) = delete;
	ScopedVertexAllocation(ScopedVertexAllocation&&) = delete;
	~ScopedVertexAllocation();

	ScopedVertexAllocation& operator=(const ScopedVertexAllocation&) = delete;
	ScopedVertexAllocation& operator=(ScopedVertexAllocation&&) = delete;

	[[nodiscard]] Vertex* CreateVertices(size_t count = 1)
	{
		return reinterpret_cast<Vertex*>(myAllocatorRef.Allocate(count));
	}

	void FreeVertices(Vertex* ptr, size_t count = 1)
	{
		myAllocatorRef.Free(reinterpret_cast<std::byte*>(ptr), count);
	}

	[[nodiscard]] VertexAllocator& Allocator() noexcept { return myAllocatorRef; }

private:
	VertexAllocator& myAllocatorRef;
	ScopedVertexAllocation* const myPrevScope = nullptr;
};
