// the D3D12 backend's buffers: committed or placed resources from D3D12MA, in the heap their use asks for: device local
// ones in the default heap, host visible ones the gpu only reads (staging, uploads) in the upload heap (the cpu's write
// combined pages), and host visible ones the gpu writes too (a storage buffer read back) in the device's custom heap of
// write back pages (backend.h's hostPool). buffers have no layout (enhanced barriers: UNDEFINED, always)
#include <rhi/d3d12/backend.h>

#include <rhi/buffer.h>
#include <rhi/device.h>
#include <rhi/memory.h>
#include <rhi/rhi.h>

#include <core/assert.h>
#include <core/profiling.h>

#include <cstring>
#include <memory>
#include <span>
#include <string_view>
#include <tuple>
#include <utility>

namespace rhi
{

IMPLEMENT_OBJECT_GETINSTANCE(Buffer<kD3D12>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(Buffer<kD3D12>);

namespace buffer
{

static D3D12_RESOURCE_DESC1 ResourceDesc(uint64_t size, BufferUsage usage)
{
	auto flags = D3D12_RESOURCE_FLAG_NONE;
	if (Any(usage & (BufferUsage::kStorage | BufferUsage::kAccelerationStructureStorage)))
		flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	if (Any(usage & BufferUsage::kAccelerationStructureStorage))
		flags |= D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE;
	return D3D12_RESOURCE_DESC1{
		.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
		.Alignment = 0,
		.Width = size,
		.Height = 1,
		.DepthOrArraySize = 1,
		.MipLevels = 1,
		.Format = DXGI_FORMAT_UNKNOWN,
		.SampleDesc = {.Count = 1, .Quality = 0},
		.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
		.Flags = flags,
		.SamplerFeedbackMipRegion = {},
	};
}

// where a buffer goes (see the file's comment)
static D3D12MA::ALLOCATION_DESC AllocationDesc(d3d12::AllocatorObject& allocator, BufferUsage usage, MemoryProperty memory)
{
	D3D12MA::ALLOCATION_DESC allocation{};
	const bool hostVisible = (memory & MemoryProperty::kHostVisible) == MemoryProperty::kHostVisible;
	const bool gpuWrites = Any(usage & (BufferUsage::kStorage | BufferUsage::kTransferDestination | BufferUsage::kAccelerationStructureStorage));
	if (!hostVisible)
		allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
	else if (!gpuWrites)
		allocation.HeapType = D3D12_HEAP_TYPE_UPLOAD;
	else
		allocation.CustomPool = allocator.hostPool;
	return allocation;
}

// a buffer (its allocation the resource's owner)
static std::tuple<ID3D12Resource*, D3D12MA::Allocation*> Create(
	ID3D12Device* device, std::string_view name, uint64_t size, BufferUsage usage, MemoryProperty memory)
{
	auto& allocator = d3d12::AllocatorObject::ForDevice(device);
	const auto allocationDesc = AllocationDesc(allocator, usage, memory);
	const auto resourceDesc = ResourceDesc(size, usage);
	D3D12MA::Allocation* allocation = nullptr;
	d3d12::Check(
		allocator.allocator->CreateResource3(
			&allocationDesc, &resourceDesc, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, 0, nullptr, &allocation, IID_NULL, nullptr),
		std::format("{}: CreateResource3 (a buffer of {} bytes)", name, size));
	d3d12::SetName(allocation->GetResource(), name);
	return {allocation->GetResource(), allocation};
}

static std::tuple<ID3D12Resource*, D3D12MA::Allocation*> Create(const BufferCreateDesc<kD3D12>& desc)
{
	return Create(desc.device, desc.name, desc.size, desc.usageFlags, desc.memoryFlags);
}

// a buffer of desc filled from source (size bytes, from its start) on cmd's list
static std::tuple<ID3D12Resource*, D3D12MA::Allocation*> CreateCopy(
	const BufferCreateDesc<kD3D12>& desc, d3d12::CommandBufferObject& cmd, ID3D12Resource* source)
{
	auto buffer = Create(desc);
	cmd.list->CopyBufferRegion(std::get<0>(buffer), 0, source, 0, desc.size);
	return buffer;
}

} // namespace buffer

template <>
Buffer<kD3D12>::Buffer(CreateDescType&& desc, ValueType&& buffer)
	: DeviceObject<Buffer<kD3D12>>(std::forward<CreateDescType>(desc))
	, myBuffer(std::forward<ValueType>(buffer))
{}

template <>
Buffer<kD3D12>::Buffer(CreateDescType&& desc)
	: Buffer(
		std::forward<CreateDescType>(desc),
		[&desc]
		{
			ZoneScopedN("Buffer::create");

			return buffer::Create(desc);
		}())
{}

template <>
void Buffer<kD3D12>::Swap(Buffer& rhs) noexcept
{
	DeviceObject<Buffer<kD3D12>>::Swap(rhs);
	std::swap(myBuffer, rhs.myBuffer);
}

template <>
Buffer<kD3D12>::Buffer(Buffer&& other) noexcept
{
	Swap(other);
}

template <>
Buffer<kD3D12>& Buffer<kD3D12>::operator=(Buffer&& other) noexcept
{
	Swap(other);
	return *this;
}

template <>
Buffer<kD3D12>::~Buffer()
{
	if (!IsValid())
		return;
	// the allocation owns its resource; a placed one (no allocation of its own) is the buffer's
	if (auto* allocation = std::get<1>(myBuffer); allocation != nullptr)
		allocation->Release();
	else if (auto* resource = std::get<0>(myBuffer); resource != nullptr)
		resource->Release();
}

template <>
std::span<std::byte> Buffer<kD3D12>::Map()
{
	auto* resource = std::get<0>(myBuffer);
	void* data = nullptr;
	d3d12::Check(resource->Map(0, nullptr, &data), "ID3D12Resource::Map (a buffer the host can't see?)");
	return {static_cast<std::byte*>(data), static_cast<size_t>(resource->GetDesc().Width)};
}

template <>
void Buffer<kD3D12>::Unmap()
{
	std::get<0>(myBuffer)->Unmap(0, nullptr);
}

// host visible memory is coherent (the upload heap's write combined pages, the custom heap's write back ones): neither
// host writes nor device writes need making visible (after the wait and the barrier)
template <>
void Buffer<kD3D12>::Flush(size_t /*offset*/, size_t /*size*/)
{}

template <>
void Buffer<kD3D12>::Invalidate(size_t /*offset*/, size_t /*size*/)
{}

template <>
uint64_t Buffer<kD3D12>::GetDeviceAddress() const
{
	return std::get<0>(myBuffer)->GetGPUVirtualAddress();
}

template <>
MemoryRequirements Buffer<kD3D12>::GetMemoryRequirements(const Device<kD3D12>& device, const CreateDescType& desc)
{
	const auto resourceDesc = buffer::ResourceDesc(desc.size, desc.usageFlags);
	const auto info = static_cast<ID3D12Device10*>(device)->GetResourceAllocationInfo2(0, 1, &resourceDesc, nullptr);
	return {.size = info.SizeInBytes, .alignment = info.Alignment, .memoryTypeBits = ~0U};
}

template <>
Buffer<kD3D12>::Buffer(CreateDescType&& desc, const MemoryBlock<kD3D12>& memory, uint64_t offset)
	: Buffer(
		std::forward<CreateDescType>(desc),
		[&desc, &memory, offset]
		{
			auto& allocator = d3d12::AllocatorObject::ForDevice(desc.device);
			const auto resourceDesc = buffer::ResourceDesc(desc.size, desc.usageFlags);
			ID3D12Resource* resource = nullptr;
			d3d12::Check(
				allocator.allocator->CreateAliasingResource2(
					memory.GetAllocation(), offset, &resourceDesc, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, 0, nullptr, IID_PPV_ARGS(&resource)),
				std::format("{}: CreateAliasingResource2 (a buffer of {} bytes at {})", desc.name, desc.size, offset));
			d3d12::SetName(resource, desc.name);
			return ValueType{resource, AllocationHandle<kD3D12>{}};
		}())
{}

template <>
Buffer<kD3D12> Buffer<kD3D12>::CreateStaging(DeviceObjectCreateDesc<kD3D12>&& desc, size_t size)
{
	return Buffer(BufferCreateDesc<kD3D12>{
		std::forward<DeviceObjectCreateDesc<kD3D12>>(desc),
		size,
		BufferUsage::kTransferSource,
		MemoryProperty::kHostVisible | MemoryProperty::kHostCoherent});
}

template <>
Buffer<kD3D12>::Buffer(
	CreateDescType&& desc,
	std::tuple<BufferHandle<kD3D12>, AllocationHandle<kD3D12>>&& initialData,
	CommandBufferHandle<kD3D12> cmd,
	core::TaskCreateInfo<void>& timelineCallbackOut)
	: Buffer(std::forward<CreateDescType>(desc), buffer::CreateCopy(desc, *cmd, std::get<0>(initialData)))
{
	// the source released once the copy's commands are done
	timelineCallbackOut = core::CreateTask(
		[source = std::get<0>(initialData), allocation = std::get<1>(initialData)]
		{
			if (allocation != nullptr)
				allocation->Release();
			else
				source->Release();
		});
}

template <>
Buffer<kD3D12>::Buffer(CreateDescType&& desc, const void* initialData, CommandBufferHandle<kD3D12> cmd, core::TaskCreateInfo<void>& timelineCallbackOut)
	: Buffer(
		std::forward<CreateDescType>(desc),
		[&desc, initialData]
		{
			// an upload buffer of the data
			auto staging = buffer::Create(desc.device, desc.name, desc.size, BufferUsage::kTransferSource, MemoryProperty::kHostVisible);
			void* data = nullptr;
			d3d12::Check(std::get<0>(staging)->Map(0, nullptr, &data), "ID3D12Resource::Map (staging)");
			std::memcpy(data, initialData, desc.size);
			std::get<0>(staging)->Unmap(0, nullptr);
			return std::tuple<BufferHandle<kD3D12>, AllocationHandle<kD3D12>>{std::get<0>(staging), std::get<1>(staging)};
		}(),
		cmd,
		timelineCallbackOut)
{}

template <>
Buffer<kD3D12>::Buffer(CreateDescType&& desc, Buffer&& staging, CommandBufferHandle<kD3D12> cmd, core::TaskCreateInfo<void>& timelineCallbackOut)
	: Buffer(std::forward<CreateDescType>(desc), buffer::CreateCopy(desc, *cmd, staging.GetBuffer()))
{
	timelineCallbackOut = core::CreateTask([staging = std::make_shared<Buffer<kD3D12>>(std::move(staging))] {});
}

} // namespace rhi
