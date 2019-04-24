#pragma once

#include <stack>
#include <vector>

#include <xxhash.h>

// todo: make sure that it will work between multiple ScopedVertexAllocation scopes
// 		 create some sort of nested data structure holding both stride and byte vector, and make sure that the scope refers to the correct one.
class VertexAllocator
{
public:
	VertexAllocator() = default;

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

	bool isLocked() const
	{
		return myIsLocked;
	}

	size_t size() const
	{
		return myData.size() / stride();
	}

	size_t sizeBytes() const
	{
		return myData.size();
	}

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

	size_t stride() const
	{
		return myStride;
	}

	const std::byte* data() const
	{
		return myData.data();
	}

	bool empty() const
	{
		return myData.size() == 0;
	}

	// todo: handle alignment properly
	// todo: rewrite without vector
	// todo: handle pointer invalidation. std::deque? needs to be flattened before upload to gpu in that case.
	std::byte* allocate(size_t count = 1)
	{
		auto bytes = count * stride();
		myData.resize(myData.size() + bytes);
		return (myData.data() + myData.size()) - bytes;
	}

	// unless freeing the last item this is _very_ inefficient
	void free(std::byte* ptr, size_t count = 1)
	{
		assert(ptr != nullptr);

		std::byte* first = myData.data();
		std::byte* last = first + myData.size();

		auto bytes = count * stride();
		assert(bytes > 0);

		std::byte* next = ptr + bytes;

		if (next == last) // is we are freeing the last item we can just resize
		{
			myData.resize(myData.size() - bytes);
		}
		else if ((next - first) % stride() != 0) // else check alignment
		{
			if ((next < last && next > first)) // and that we are inside the right range
			{
				// copy over this item with rest of range, resize down to right size.
				std::copy(next, last, ptr);
				myData.resize(myData.size() - bytes);
			}
		}
	}

	void clear()
	{
		myData.clear();
		myStride = 0;
		myIsLocked = false;
	}

	template <class Archive>
	void serialize(Archive& ar)
	{
		ar(myData);
		ar(myStride);
	}

private:
	std::vector<std::byte> myData;
	size_t myStride = 0;
	bool myIsLocked = false;
};

class ScopedVertexAllocation;

// todo: alignment for sse types, etc
class Vertex
{
public:
	inline bool operator==(const Vertex& other) const
	{
		return std::equal(
			this,
			reinterpret_cast<const Vertex*>(
				reinterpret_cast<const std::byte*>(this) + allocator().stride()),
			&other);
	}

	inline uint64_t hash(size_t seed = 42) const
	{
		return XXH64(reinterpret_cast<const std::byte*>(this), allocator().stride(), seed);
	}

	template <typename T>
	inline T* dataAs(size_t offset = 0)
	{
		return reinterpret_cast<T*>(reinterpret_cast<std::byte*>(this) + offset);
	}

	template <typename T>
	inline const T* dataAs(size_t offset = 0) const
	{
		return reinterpret_cast<const T*>(reinterpret_cast<const std::byte*>(this) + offset);
	}

	inline static ScopedVertexAllocation* const getScope()
	{
		return st_allocationScope;
	}

private:
	friend ScopedVertexAllocation;

	static VertexAllocator& allocator();

	inline static void setScope(ScopedVertexAllocation* scope)
	{
		st_allocationScope = scope;
	}

	Vertex() = delete;
	~Vertex() = delete;
	Vertex(const Vertex& other) = delete;
	Vertex& operator=(const Vertex& other) = delete;

	static thread_local ScopedVertexAllocation* st_allocationScope;
};

static_assert(sizeof(Vertex) == std::alignment_of_v<Vertex>);

thread_local ScopedVertexAllocation* Vertex::st_allocationScope = nullptr;

class ScopedVertexAllocation : Noncopyable
{
public:
	inline ScopedVertexAllocation(VertexAllocator& allocator)
		: myAllocatorRef(allocator)
		, myPrevScope(Vertex::getScope())
	{
		myAllocatorRef.lock();
		Vertex::setScope(this);
	}

	inline ~ScopedVertexAllocation()
	{
		Vertex::setScope(myPrevScope);
		myAllocatorRef.unlock();
	}

	inline Vertex* createVertices(size_t count = 1)
	{
		return reinterpret_cast<Vertex*>(myAllocatorRef.allocate(count));
	}

	inline void freeVertices(Vertex* ptr, size_t count = 1)
	{
		return myAllocatorRef.free(reinterpret_cast<std::byte*>(ptr), count);
	}

	inline VertexAllocator& allocator()
	{
		return myAllocatorRef;
	}

private:
	VertexAllocator& myAllocatorRef;
	ScopedVertexAllocation* const myPrevScope = nullptr;
};

VertexAllocator& Vertex::allocator()
{
	assert(st_allocationScope != nullptr);
	return st_allocationScope->allocator();
}

namespace std
{
template <>
struct hash<Vertex>
{
	inline size_t operator()(Vertex const& vertex) const
	{
		return vertex.hash();
	}
};
} // namespace std
