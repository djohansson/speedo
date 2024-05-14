// TODO: remove vertex.h/inl/cpp
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

	bool IsLocked() const { return myIsLocked; }
	size_t Size() const { return myData.size() / Stride(); }
	size_t SizeBytes() const { return myData.size(); }

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

	size_t Stride() const { return myStride; }
	const void* Data() const { return myData.data(); }
	bool Empty() const { return myData.size() == 0; }

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
public:
	bool operator==(const Vertex& other) const
	{
		return std::equal(
			this,
			reinterpret_cast<const Vertex*>(
				reinterpret_cast<const std::byte*>(this) + Allocator().Stride()),
			&other);
	}

	uint64_t Hash(size_t seed = 42) const
	{
		return XXH64(reinterpret_cast<const std::byte*>(this), Allocator().Stride(), seed);
	}

	template <typename T>
	T& DataAs(size_t offset = 0)
	{
		return *reinterpret_cast<T*>(reinterpret_cast<std::byte*>(this) + offset);
	}

	template <typename T>
	const T& DataAs(size_t offset = 0) const
	{
		return *reinterpret_cast<const T*>(reinterpret_cast<const std::byte*>(this) + offset);
	}

	static ScopedVertexAllocation* const GetScope() { return gAllocationScope; }

private:
	friend ScopedVertexAllocation;

	static VertexAllocator& Allocator();

	static void SetScope(ScopedVertexAllocation* scope) { gAllocationScope = scope; }

	Vertex() = delete;
	~Vertex() = delete;
	Vertex(const Vertex& other) = delete;
	Vertex& operator=(const Vertex& other) = delete;

	static thread_local ScopedVertexAllocation* gAllocationScope;
};

static_assert(sizeof(Vertex) == std::alignment_of_v<Vertex>);

class ScopedVertexAllocation
	: Noncopyable
	, Nondynamic
{
public:
	ScopedVertexAllocation(VertexAllocator& allocator);
	~ScopedVertexAllocation();

	Vertex* CreateVertices(size_t count = 1)
	{
		return reinterpret_cast<Vertex*>(myAllocatorRef.Allocate(count));
	}

	void FreeVertices(Vertex* ptr, size_t count = 1)
	{
		return myAllocatorRef.Free(reinterpret_cast<std::byte*>(ptr), count);
	}

	VertexAllocator& Allocator() { return myAllocatorRef; }

private:
	VertexAllocator& myAllocatorRef;
	ScopedVertexAllocation* const myPrevScope = nullptr;
};
