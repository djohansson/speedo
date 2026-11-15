// the D3D12 backend's memory blocks: D3D12MA's allocations of memory alone, buffers and images placed at offsets in them
// (their constructors that take a memory block: CreateAliasingResource2). resources aliasing the same bytes are ordered
// by the frame graph's barriers between the passes that use them
#include <rhi/d3d12/backend.h>

#include <rhi/memory.h>

#include <core/assert.h>

#include <format>

namespace rhi
{

template <>
MemoryBlock<kD3D12>::MemoryBlock(AllocatorHandle<kD3D12> allocator, const MemoryRequirements& requirements, MemoryProperty flags, const char* name)
	: myAllocator(allocator)
	, mySize(requirements.size)
{
	ENSURE(allocator != nullptr);
	D3D12MA::ALLOCATION_DESC allocationDesc{};
	// a heap of its own (placed resources need the heap), host visible where asked (the custom heap's pool)
	allocationDesc.Flags = D3D12MA::ALLOCATION_FLAG_COMMITTED;
	if ((flags & MemoryProperty::kHostVisible) == MemoryProperty::kHostVisible)
		allocationDesc.CustomPool = allocator->hostPool;
	else
		allocationDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;
	const D3D12_RESOURCE_ALLOCATION_INFO info{.SizeInBytes = requirements.size, .Alignment = requirements.alignment};
	d3d12::Check(
		allocator->allocator->AllocateMemory(&allocationDesc, &info, &myAllocation),
		std::format("{}: AllocateMemory ({} bytes)", name != nullptr ? name : "a memory block", requirements.size));
	if (name != nullptr)
	{
		const std::string text(name);
		const std::wstring wide(text.begin(), text.end());
		myAllocation->SetName(wide.c_str());
	}
}

template <>
MemoryBlock<kD3D12>::~MemoryBlock()
{
	if (myAllocation != nullptr)
		myAllocation->Release();
}

} // namespace rhi
