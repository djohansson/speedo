// TODO: remove vertex.h/inl/cpp
#pragma once

#include "types.h"

#include <core/utils.h>

#include <stack>
#include <vector>

#include <xxhash.h>

// todo: delete this whole file - use gltf instead
class VertexAllocator
{
public:
	VertexAllocator() noexcept = default;

	void lock()
	{
		assert(!myIsLocked);

		myIsLocked = true;
	}

	void unlock()
	{
		assert(myIsLocked);

		myIsLocked = false;
	}

	bool isLocked() const { return myIsLocked; }

	size_t size() const { return myData.size() / stride(); }

	size_t sizeBytes() const { return myData.size(); }

	void reserve(size_t size)
	{
		assert(myIsLocked);

		myData.reserve(size * stride());
	}

	void setStride(size_t stride)
	{
		assert(!myIsLocked);

		myStride = stride;
	}

	size_t stride() const { return myStride; }

	const void* data() const { return myData.data(); }

	bool empty() const { return myData.size() == 0; }

	// todo: handle alignment properly
	// todo: rewrite without vector
	// todo: handle pointer invalidation. std::deque? needs to be flattened before upload to gpu in that case.
	void* allocate(size_t count = 1);

	// unless freeing the last item this is _very_ inefficient
	void deallocate(void* ptr, size_t count = 1);

	void clear();

	template <class Archive>
	void serialize(Archive& archive)
	{
		archive(myData);
		archive(myStride);
	}

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
				reinterpret_cast<const std::byte*>(this) + allocator().stride()),
			&other);
	}

	uint64_t hash(size_t seed = 42) const
	{
		return XXH64(reinterpret_cast<const std::byte*>(this), allocator().stride(), seed);
	}

	template <typename T>
	T* dataAs(size_t offset = 0)
	{
		return reinterpret_cast<T*>(reinterpret_cast<std::byte*>(this) + offset);
	}

	template <typename T>
	const T* dataAs(size_t offset = 0) const
	{
		return reinterpret_cast<const T*>(reinterpret_cast<const std::byte*>(this) + offset);
	}

	static ScopedVertexAllocation* const getScope() { return st_allocationScope; }

private:
	friend ScopedVertexAllocation;

	static VertexAllocator& allocator();

	static void setScope(ScopedVertexAllocation* scope) { st_allocationScope = scope; }

	Vertex() = delete;
	~Vertex() = delete;
	Vertex(const Vertex& other) = delete;
	Vertex& operator=(const Vertex& other) = delete;

	static thread_local ScopedVertexAllocation* st_allocationScope;
};

static_assert(sizeof(Vertex) == std::alignment_of_v<Vertex>);

class ScopedVertexAllocation
	: Noncopyable
	, Nondynamic
{
public:
	ScopedVertexAllocation(VertexAllocator& allocator);
	~ScopedVertexAllocation();

	Vertex* createVertices(size_t count = 1)
	{
		return reinterpret_cast<Vertex*>(myAllocatorRef.allocate(count));
	}

	void freeVertices(Vertex* ptr, size_t count = 1)
	{
		return myAllocatorRef.deallocate(reinterpret_cast<std::byte*>(ptr), count);
	}

	VertexAllocator& allocator() { return myAllocatorRef; }

private:
	VertexAllocator& myAllocatorRef;
	ScopedVertexAllocation* const myPrevScope = nullptr;
};

#include "vertex.inl"
