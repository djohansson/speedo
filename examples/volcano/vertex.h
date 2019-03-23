#pragma once

#include <vector>

#include <xxhash.h>

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
	void* push_back(size_t count = 1)
	{
		auto bytes = count * stride();
		myData.resize(myData.size() + bytes);
		return (myData.data() + myData.size()) - bytes;
	}

	// todo: handle alignment properly
	// todo: handle out of bounds
	void pop_back(size_t count = 1)
	{
		auto bytes = count * stride();
		myData.resize(myData.size() - bytes);
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

class ScopedVertexAllocation : Noncopyable
{
public:
	ScopedVertexAllocation(VertexAllocator& allocator_)
		: allocator(allocator_)
	{
		allocator.lock();
	}

	~ScopedVertexAllocation()
	{
		allocator.unlock();
	}

private:
	VertexAllocator& allocator;
};

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

	inline static VertexAllocator& allocator()
	{
		return st_allocator;
	}

	inline static Vertex& create()
	{
		return *reinterpret_cast<Vertex*>(allocator().push_back());
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

private:
	Vertex() = delete;
	~Vertex() = delete;
	Vertex(const Vertex& other) = delete;
	Vertex& operator=(const Vertex& other) = delete;

	static thread_local VertexAllocator st_allocator;
};

static_assert(sizeof(Vertex) == std::alignment_of_v<Vertex>);

thread_local VertexAllocator Vertex::st_allocator{};

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
