// the D3D12 backend's command pools and command buffers: a command buffer is a command allocator of its own and a command
// list (an allocator records one list at a time, and a pool's command buffers may record at once), taken from the pool's
// free ones or made, and given back to them when the pool is reset (its commands done: Queue::SwapAndResetPool), which
// resets its allocator and frees what its commands held (backend.h's CommandBufferObject)
#include <rhi/d3d12/backend.h>

#include <rhi/command.h>
#include <rhi/device.h>
#include <rhi/rhi.h>

#include <core/assert.h>
#include <core/profiling.h>

#include <algorithm>
#include <array>
#include <print>
#include <utility>

namespace rhi
{

namespace d3d12
{

void CommandBufferObject::PrepareDispatch()
{
	ENSUREF(pipeline != nullptr && pipeline->compute, "a dispatch without a compute pipeline bound (Pipeline::BindPipelineAuto)");
	if (computeRootSignature != pipeline->rootSignature)
	{
		list->SetComputeRootSignature(pipeline->rootSignature);
		computeRootSignature = pipeline->rootSignature;
	}
	list->SetPipelineState(pipeline->state);
	ENSURE(layout != nullptr);
	if (layout->pushConstants != PipelineLayoutObject::kNone)
	{
		pushConstants.resize(layout->pushConstantCount);
		list->SetComputeRoot32BitConstants(layout->pushConstants, layout->pushConstantCount, pushConstants.data(), 0);
	}
	for (uint32_t set = 0; set < layout->sets.size(); set++)
	{
		const auto& tables = layout->sets[set];
		if (tables.resources == PipelineLayoutObject::kNone && tables.samplers == PipelineLayoutObject::kNone)
			continue;
		ENSUREF(set < sets.size() && (sets[set].resources.ptr != 0 || sets[set].samplers.ptr != 0), "a dispatch without set {} bound", set);
		if (tables.resources != PipelineLayoutObject::kNone)
			list->SetComputeRootDescriptorTable(tables.resources, sets[set].resources);
		if (tables.samplers != PipelineLayoutObject::kNone)
			list->SetComputeRootDescriptorTable(tables.samplers, sets[set].samplers);
	}
}

void CommandBufferObject::PrepareDraw()
{
	ENSUREF(pipeline != nullptr && !pipeline->compute, "a draw without a graphics pipeline bound (Pipeline::BindPipelineAuto)");
	if (graphicsRootSignature != pipeline->rootSignature)
	{
		list->SetGraphicsRootSignature(pipeline->rootSignature);
		graphicsRootSignature = pipeline->rootSignature;
	}
	list->SetPipelineState(pipeline->state);
	list->IASetPrimitiveTopology(pipeline->topology);
	ENSURE(layout != nullptr);
	if (layout->pushConstants != PipelineLayoutObject::kNone)
	{
		pushConstants.resize(layout->pushConstantCount);
		list->SetGraphicsRoot32BitConstants(layout->pushConstants, layout->pushConstantCount, pushConstants.data(), 0);
	}
	for (uint32_t set = 0; set < layout->sets.size(); set++)
	{
		const auto& tables = layout->sets[set];
		if (tables.resources == PipelineLayoutObject::kNone && tables.samplers == PipelineLayoutObject::kNone)
			continue;
		ENSUREF(set < sets.size() && (sets[set].resources.ptr != 0 || sets[set].samplers.ptr != 0), "a draw without set {} bound", set);
		if (tables.resources != PipelineLayoutObject::kNone)
			list->SetGraphicsRootDescriptorTable(tables.resources, sets[set].resources);
		if (tables.samplers != PipelineLayoutObject::kNone)
			list->SetGraphicsRootDescriptorTable(tables.samplers, sets[set].samplers);
	}
}

void CommandBufferObject::FreeTransients() noexcept
{
	for (const auto& [offset, count] : pushedResources)
		device->pool.resources.Free(offset, count);
	for (const auto& [offset, count] : pushedSamplers)
		device->pool.samplers.Free(offset, count);
	pushedResources.clear();
	pushedSamplers.clear();
	for (auto*& resource : transientResources)
		Release(resource);
	transientResources.clear();
	for (auto*& allocation : transients)
		Release(allocation);
	transients.clear();
}

} // namespace d3d12

namespace commandpool
{

// the kinds of command buffer a pool keeps apart (indices into its lists)
constexpr size_t kPrimary = 0;
constexpr size_t kSecondary = 1;

void Free(d3d12::CommandBufferObject* object)
{
	object->FreeTransients();
	d3d12::Release(object->list);
	d3d12::Release(object->allocator);
	delete object; //NOLINT(cppcoreguidelines-owning-memory): the handle is the pool's
}

} // namespace commandpool

IMPLEMENT_OBJECT_GETINSTANCE(CommandPool<kD3D12>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(CommandPool<kD3D12>);

// defined before CommandBuffer's members, which use it (see CommandBuffer::Release)
template <>
void CommandPool<kD3D12>::Retire(CommandBufferHandle<kD3D12> handle, bool secondary)
{
	using namespace commandpool;

	ENSURE(myOutstanding > 0);
	myOutstanding--;
	myUsed[secondary ? kSecondary : kPrimary].push_back(handle);
}

template <>
void CommandBuffer<kD3D12>::End()
{
	ZoneScopedN("CommandBuffer::End");

	ENSURE(myRecording);
	myRecording = false;
	myHandle->recording = false;
	d3d12::Check(myHandle->list->Close(), "ID3D12GraphicsCommandList::Close");
}

template <>
CommandBuffer<kD3D12>::~CommandBuffer()
{
	if (!IsValid())
		return;

#if (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
	std::println(stderr, "a command buffer was dropped without being submitted: its commands won't run");
#endif
	(void)Release();
}

template <>
CommandPool<kD3D12>::CommandPool(CreateDescType&& desc, CommandPoolHandle<kD3D12>&& pool)
	: DeviceObject<CommandPool<kD3D12>>(std::forward<CreateDescType>(desc))
	, myPool(std::forward<CommandPoolHandle<kD3D12>>(pool))
{}

template <>
CommandPool<kD3D12>::CommandPool(CreateDescType&& desc)
	: CommandPool(
		std::forward<CreateDescType>(desc),
		new d3d12::CommandPoolObject{.device = desc.device, .type = d3d12::ToD3D12(desc.queueFamilyIndex)}) //NOLINT(cppcoreguidelines-owning-memory)
{}

template <>
void CommandPool<kD3D12>::Swap(CommandPool& other) noexcept
{
	DeviceObject<CommandPool<kD3D12>>::Swap(other);
	std::swap(myPool, other.myPool);
	std::swap(myFree, other.myFree);
	std::swap(myUsed, other.myUsed);
	std::swap(myOutstanding, other.myOutstanding);
}

template <>
CommandPool<kD3D12>::CommandPool(CommandPool&& other) noexcept
{
	Swap(other);
}

template <>
CommandPool<kD3D12>& CommandPool<kD3D12>::operator=(CommandPool&& other) noexcept
{
	Swap(other);
	return *this;
}

template <>
CommandPool<kD3D12>::~CommandPool()
{
	if (!IsValid())
		return;

	for (auto& handles : myUsed)
		for (auto* handle : handles)
			commandpool::Free(handle);
	for (auto& handles : myFree)
		for (auto* handle : handles)
			commandpool::Free(handle);
	delete myPool; //NOLINT(cppcoreguidelines-owning-memory)
}

template <>
void CommandPool<kD3D12>::Reset()
{
	ZoneScopedN("CommandPool::Reset");

	ENSUREF(myOutstanding == 0, "resetting a command pool while {} of its command buffers are held", myOutstanding);

	// their commands are done: each allocator reset, what they held freed, and each free for its next use
	for (size_t kind = 0; kind < myUsed.size(); kind++)
	{
		for (auto* handle : myUsed[kind])
		{
			handle->FreeTransients();
			d3d12::Check(handle->allocator->Reset(), "ID3D12CommandAllocator::Reset");
			myFree[kind].push_back(handle);
		}
		myUsed[kind].clear();
	}
}

template <>
CommandBufferHandle<kD3D12> CommandPool<kD3D12>::InternalTake(bool secondary)
{
	using namespace commandpool;

	auto& free = myFree[secondary ? kSecondary : kPrimary];
	d3d12::CommandBufferObject* object = nullptr;
	if (!free.empty())
	{
		object = free.back();
		free.pop_back();
		d3d12::Check(object->list->Reset(object->allocator, nullptr), "ID3D12GraphicsCommandList::Reset");
	}
	else
	{
		object = new d3d12::CommandBufferObject{}; //NOLINT(cppcoreguidelines-owning-memory): freed by the pool
		object->type = myPool->type;
		object->device = &d3d12::AllocatorObject::ForDevice(myPool->device);
		d3d12::Check(myPool->device->CreateCommandAllocator(myPool->type, IID_PPV_ARGS(&object->allocator)), "CreateCommandAllocator");
		d3d12::Check(
			myPool->device->CreateCommandList(0, myPool->type, object->allocator, nullptr, IID_PPV_ARGS(&object->list)),
			"CreateCommandList");
		d3d12::SetName(object->list, GetName());
	}
	// a fresh list's state: nothing set, the shader visible heaps (a copy list takes none)
	object->recording = true;
	object->pipeline = nullptr;
	object->computeRootSignature = nullptr;
	object->graphicsRootSignature = nullptr;
	object->layout = nullptr;
	object->pushConstants.clear();
	object->sets.clear();
	if (object->type != D3D12_COMMAND_LIST_TYPE_COPY)
	{
		std::array<ID3D12DescriptorHeap*, 2> heaps{object->device->pool.resources.GetHeap(), object->device->pool.samplers.GetHeap()};
		object->list->SetDescriptorHeaps(static_cast<UINT>(heaps.size()), heaps.data());
	}
	myOutstanding++;
	return object;
}

template <>
CommandBuffer<kD3D12> CommandPool<kD3D12>::Begin()
{
	return CommandBuffer<kD3D12>(*this, InternalTake(false), false);
}

template <>
CommandBufferHandle<kD3D12> CommandPool<kD3D12>::Allocate()
{
	return InternalTake(false);
}

template <>
CommandBuffer<kD3D12> CommandPool<kD3D12>::BeginSecondary(const RenderTargetBeginInfo<kD3D12>& /*renderTarget*/)
{
	ENSUREF(false, "the D3D12 backend has no secondary command buffers yet (docs/backends-plan.md, step 3)");
	return {};
}

} // namespace rhi
