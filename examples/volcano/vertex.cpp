#include "vertex.h"

std::byte* VertexAllocator::allocate(size_t count)
{
    auto bytes = count * stride();
    myData.resize(myData.size() + bytes);
    return (myData.data() + myData.size()) - bytes;
}

void VertexAllocator::free(std::byte* ptr, size_t count)
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
    assert(st_allocationScope != nullptr);
    return st_allocationScope->allocator();
}

thread_local ScopedVertexAllocation* Vertex::st_allocationScope = nullptr;

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
