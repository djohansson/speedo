// the D3D12 backend's query pools of timestamps: a query heap, each timestamp resolved where it is written
// (ResolveQueryData right after its EndQuery, on the same list) into a readback buffer of the values, which the host reads
// once the commands are done. A reset sets the values to QueryPoolObject::kUnwritten (by the host, or recorded: a copy
// from an upload buffer of them), so that a read tells which were written since (no tick count reaches it). The ticks are
// the queue's (DeviceLimits::timestampPeriod: the direct queue's frequency). acceleration structures' compacted sizes
// come with the ray queries (docs/backends-plan.md, step 6)
#include <rhi/d3d12/backend.h>

#include <rhi/querypool.h>
#include <rhi/rhi.h>

#include <core/assert.h>

#include <algorithm>
#include <atomic>
#include <format>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace rhi
{

IMPLEMENT_OBJECT_GETINSTANCE(QueryPool<kD3D12>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(QueryPool<kD3D12>);

namespace querypool
{

// a buffer of size bytes in heapType's memory, mapped
static std::pair<D3D12MA::Allocation*, void*> CreateMapped(ID3D12Device* device, D3D12_HEAP_TYPE heapType, uint64_t size, std::string_view name)
{
	auto& allocator = d3d12::AllocatorObject::ForDevice(device);
	D3D12MA::ALLOCATION_DESC allocationDesc{};
	allocationDesc.HeapType = heapType;
	const D3D12_RESOURCE_DESC1 desc{
		.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
		.Alignment = 0,
		.Width = size,
		.Height = 1,
		.DepthOrArraySize = 1,
		.MipLevels = 1,
		.Format = DXGI_FORMAT_UNKNOWN,
		.SampleDesc = {.Count = 1, .Quality = 0},
		.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
		.Flags = D3D12_RESOURCE_FLAG_NONE,
		.SamplerFeedbackMipRegion = {},
	};
	D3D12MA::Allocation* allocation = nullptr;
	d3d12::Check(
		allocator.allocator->CreateResource3(
			&allocationDesc, &desc, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, 0, nullptr, &allocation, IID_NULL, nullptr),
		std::format("{}: CreateResource3 (a buffer of {} bytes)", name, size));
	d3d12::SetName(allocation->GetResource(), name);
	void* data = nullptr;
	d3d12::Check(allocation->GetResource()->Map(0, nullptr, &data), std::format("{}: ID3D12Resource::Map", name));
	return {allocation, data};
}

} // namespace querypool

template <>
QueryPool<kD3D12>::QueryPool(CreateDescType&& desc, QueryPoolHandle<kD3D12>&& pool)
	: DeviceObject<QueryPool<kD3D12>>(std::forward<CreateDescType>(desc))
	, myPool(std::forward<QueryPoolHandle<kD3D12>>(pool))
{}

template <>
QueryPool<kD3D12>::QueryPool(CreateDescType&& desc)
	: QueryPool(
		std::forward<CreateDescType>(desc),
		[&desc]
		{
			ENSURE(desc.count > 0);
			ENSUREF(
				desc.type == QueryType::kTimestamp,
				"the D3D12 backend's query pools hold timestamps alone until its acceleration structures (docs/backends-plan.md, step 6)");

			auto* pool = new d3d12::QueryPoolObject{}; //NOLINT(cppcoreguidelines-owning-memory)
			const D3D12_QUERY_HEAP_DESC heapDesc{.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP, .Count = desc.count, .NodeMask = 0};
			d3d12::Check(desc.device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&pool->heap)), std::format("{}: CreateQueryHeap", desc.name));
			d3d12::SetName(pool->heap, desc.name);

			const uint64_t size = uint64_t{desc.count} * sizeof(uint64_t);
			auto [readback, values] =
				querypool::CreateMapped(desc.device, D3D12_HEAP_TYPE_READBACK, size, std::format("{} (values)", desc.name));
			pool->readback = readback;
			pool->values = static_cast<uint64_t*>(values);
			auto [unwritten, unwrittenValues] =
				querypool::CreateMapped(desc.device, D3D12_HEAP_TYPE_UPLOAD, size, std::format("{} (unwritten)", desc.name));
			pool->unwritten = unwritten;
			std::ranges::fill(std::span(static_cast<uint64_t*>(unwrittenValues), desc.count), d3d12::QueryPoolObject::kUnwritten);
			unwritten->GetResource()->Unmap(0, nullptr);
			std::ranges::fill(std::span(pool->values, desc.count), d3d12::QueryPoolObject::kUnwritten);
			return pool;
		}())
{}

template <>
void QueryPool<kD3D12>::Swap(QueryPool& rhs) noexcept
{
	DeviceObject<QueryPool<kD3D12>>::Swap(rhs);
	std::swap(myPool, rhs.myPool);
}

template <>
QueryPool<kD3D12>::QueryPool(QueryPool&& other) noexcept
{
	Swap(other);
}

template <>
QueryPool<kD3D12>::~QueryPool()
{
	if (!IsValid() || myPool == nullptr)
		return;
	d3d12::Release(myPool->heap);
	if (myPool->readback != nullptr)
		myPool->readback->Release();
	if (myPool->unwritten != nullptr)
		myPool->unwritten->Release();
	delete myPool; //NOLINT(cppcoreguidelines-owning-memory)
}

template <>
QueryPool<kD3D12>& QueryPool<kD3D12>::operator=(QueryPool&& other) noexcept
{
	Swap(other);
	return *this;
}

template <>
void QueryPool<kD3D12>::Reset(uint32_t first, uint32_t count) const
{
	ENSURE(uint64_t{first} + count <= GetDesc().count);
	std::ranges::fill(std::span(myPool->values + first, count), d3d12::QueryPoolObject::kUnwritten);
}

template <>
void QueryPool<kD3D12>::Reset(CommandBufferHandle<kD3D12> cmd, uint32_t first, uint32_t count) const
{
	ENSURE(uint64_t{first} + count <= GetDesc().count);
	const uint64_t offset = uint64_t{first} * sizeof(uint64_t);
	cmd->list->CopyBufferRegion(
		myPool->readback->GetResource(), offset, myPool->unwritten->GetResource(), offset, uint64_t{count} * sizeof(uint64_t));
	// before the timestamps resolved after it (copies both)
	const D3D12_BUFFER_BARRIER barrier{
		.SyncBefore = D3D12_BARRIER_SYNC_COPY,
		.SyncAfter = D3D12_BARRIER_SYNC_COPY,
		.AccessBefore = D3D12_BARRIER_ACCESS_COPY_DEST,
		.AccessAfter = D3D12_BARRIER_ACCESS_COPY_DEST,
		.pResource = myPool->readback->GetResource(),
		.Offset = 0,
		.Size = UINT64_MAX,
	};
	const D3D12_BARRIER_GROUP group{.Type = D3D12_BARRIER_TYPE_BUFFER, .NumBarriers = 1, .pBufferBarriers = &barrier};
	cmd->list->Barrier(1, &group);
}

template <>
void QueryPool<kD3D12>::WriteTimestamp(CommandBufferHandle<kD3D12> cmd, uint32_t index, PipelineStage /*stage*/) const
{
	// D3D12's timestamps are written once the commands before are done (bottom of pipe, whatever the stage)
	ENSURE(index < GetDesc().count);
	cmd->list->EndQuery(myPool->heap, D3D12_QUERY_TYPE_TIMESTAMP, index);
	cmd->list->ResolveQueryData(
		myPool->heap, D3D12_QUERY_TYPE_TIMESTAMP, index, 1, myPool->readback->GetResource(), uint64_t{index} * sizeof(uint64_t));
}

template <>
void QueryPool<kD3D12>::Read(uint32_t first, std::span<std::optional<uint64_t>> timestamps) const
{
	ENSURE(first + timestamps.size() <= GetDesc().count);
	for (size_t queryIt = 0; queryIt < timestamps.size(); queryIt++)
	{
		const uint64_t value = std::atomic_ref(myPool->values[first + queryIt]).load(std::memory_order_acquire);
		timestamps[queryIt].reset();
		if (value != d3d12::QueryPoolObject::kUnwritten)
			timestamps[queryIt] = value;
	}
}

} // namespace rhi
