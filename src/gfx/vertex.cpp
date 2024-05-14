// TODO(djohansson): remove vertex.h/inl/cpp
#include "vertex.h"

void* VertexAllocator::allocate(size_t count)
{
	auto bytes = count * stride();
	myData.resize(myData.size() + bytes);
	return (myData.data() + myData.size()) - bytes;
}

void VertexAllocator::free(void* ptr, size_t count)
{
	ASSERT(ptr != nullptr);

	auto* first = myData.data();
	auto* last = first + myData.size();

	auto bytes = count * stride();
	ASSERT(bytes > 0);

	auto* next = static_cast<char*>(ptr) + bytes;

	if (next == last) // is we are freeing the last item we can just resize
	{
		myData.resize(myData.size() - bytes);
	}
	else if ((next - first) % stride() != 0) // else check alignment
	{
		if ((next < last && next > first)) // and that we are inside the right range
		{
			// copy over this item with rest of range, resize down to right size.
			std::copy(next, last, static_cast<char*>(ptr));
			myData.resize(myData.size() - bytes);
		}
	}
}

void VertexAllocator::clear()
{
	myData.clear();
	myStride = 0;
	myIsLocked = false;
}

ScopedVertexAllocation::ScopedVertexAllocation(VertexAllocator& allocator)
	: myAllocatorRef(allocator)
	, myPrevScope(Vertex::getScope())
{
	myAllocatorRef.lock();
	Vertex::setScope(this);
}

ScopedVertexAllocation::~ScopedVertexAllocation()
{
	Vertex::setScope(myPrevScope);
	myAllocatorRef.unlock();
}

VertexAllocator& Vertex::allocator()
{
	ASSERT(st_allocationScope != nullptr);
	return st_allocationScope->allocator();
}

thread_local ScopedVertexAllocation* Vertex::st_allocationScope = nullptr;

namespace std
{

template <>
struct hash<Vertex>
{
	size_t operator()(Vertex const& vertex) const { return vertex.hash(); }
};

} // namespace std
