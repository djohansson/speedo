// TODO(djohansson): remove vertex.h/inl/cpp
#include "vertex.h"

void* VertexAllocator::Allocate(size_t count)
{
	auto bytes = count * Stride();
	myData.resize(myData.size() + bytes);
	return (myData.data() + myData.size()) - bytes;
}

void VertexAllocator::Free(void* ptr, size_t count)
{
	ASSERT(ptr != nullptr);

	auto* first = myData.data();
	auto* last = first + myData.size();

	auto bytes = count * Stride();
	ASSERT(bytes > 0);

	auto* next = static_cast<char*>(ptr) + bytes;

	if (next == last) // is we are freeing the last item we can just resize
	{
		myData.resize(myData.size() - bytes);
	}
	else if ((next - first) % Stride() != 0) // else check alignment
	{
		if ((next < last && next > first)) // and that we are inside the right range
		{
			// copy over this item with rest of range, resize down to right size.
			std::copy(next, last, static_cast<char*>(ptr));
			myData.resize(myData.size() - bytes);
		}
	}
}

void VertexAllocator::Clear()
{
	myData.clear();
	myStride = 0;
	myIsLocked = false;
}

ScopedVertexAllocation::ScopedVertexAllocation(VertexAllocator& allocator)
	: myAllocatorRef(allocator)
	, myPrevScope(Vertex::GetScope())
{
	myAllocatorRef.Lock();
	Vertex::SetScope(this);
}

ScopedVertexAllocation::~ScopedVertexAllocation()
{
	Vertex::SetScope(myPrevScope);
	myAllocatorRef.Unlock();
}

VertexAllocator& Vertex::Allocator()
{
	ASSERT(gAllocationScope != nullptr);
	return gAllocationScope->Allocator();
}

thread_local ScopedVertexAllocation* Vertex::gAllocationScope = nullptr;

//NOLINTBEGIN(readability-identifier-naming)

namespace std
{

template <>
struct hash<Vertex>
{
	size_t operator()(Vertex const& vertex) const { return vertex.Hash(); }
};

} // namespace std

//NOLINTEND(readability-identifier-naming)
