// the Metal backend's memory blocks: placement heaps, buffers and images placed at offsets in them (Buffer's and Image's
// constructors that take a memory block), hazard tracked as the backend's other resources are. resources aliasing the
// same bytes are ordered by the frame graph's barriers between the passes that use them
#include <rhi/metal/backend.h>

#include <rhi/memory.h>

#include <core/assert.h>

namespace rhi
{

template <>
MemoryBlock<kMetal>::MemoryBlock(AllocatorHandle<kMetal> allocator, const MemoryRequirements& requirements, MemoryProperty flags, const char* name)
	: myAllocator(allocator)
	, mySize(requirements.size)
{
	ENSURE(allocator != nullptr);
	auto* descriptor = MTL::HeapDescriptor::alloc()->init();
	descriptor->setType(MTL::HeapTypePlacement);
	descriptor->setSize(requirements.size);
	const bool hostVisible = (flags & MemoryProperty::kHostVisible) == MemoryProperty::kHostVisible;
	descriptor->setStorageMode(hostVisible ? MTL::StorageModeShared : MTL::StorageModePrivate);
	descriptor->setHazardTrackingMode(MTL::HazardTrackingModeTracked);
	MTL::Heap* heap = allocator->device->newHeap(descriptor);
	descriptor->release();
	ENSUREF(heap != nullptr, "Failed to create a Metal heap of {} bytes", requirements.size);
	if (name != nullptr)
		heap->setLabel(NS::String::string(name, NS::UTF8StringEncoding));
	myAllocation = new metal::AllocationObject{.heap = heap};
}

template <>
MemoryBlock<kMetal>::~MemoryBlock()
{
	if (myAllocation == nullptr)
		return;
	myAllocation->heap->release();
	delete myAllocation;
}

} // namespace rhi
