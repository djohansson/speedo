// the D3D12 backend's queue: an ID3D12CommandQueue of its family's type (direct, compute, copy), and its submissions on
// the timelines (D3D12 fences, a timeline semaphore's model): a submission's waits are the queue's waits on the fences,
// its command lists executed, then its signals
#include <rhi/d3d12/backend.h>

#include <rhi/crashreport.h>
#include <rhi/device.h>
#include <rhi/queue.h>
#include <rhi/rhi.h>

#include <core/assert.h>
#include <core/profiling.h>

#include <algorithm>
#include <format>
#include <memory>
#include <utility>
#include <vector>

namespace rhi
{

IMPLEMENT_OBJECT_GETINSTANCE(Queue<kD3D12>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(Queue<kD3D12>);

template <>
Queue<kD3D12>::Queue(CreateDescType&& desc, QueueHandle<kD3D12>&& handle)
	: DeviceObject<Queue<kD3D12>>(std::forward<CreateDescType>(desc))
	, myQueue(std::forward<QueueHandle<kD3D12>>(handle))
	, myPools({
		  CommandPool<kD3D12>(CommandPoolCreateDesc<kD3D12>{
			  SuperType::CreateDeviceObjectCreateDesc("CommandPool 0"), 0, GetDesc().queueFamilyIndex, myQueue}),
		  CommandPool<kD3D12>(CommandPoolCreateDesc<kD3D12>{
			  SuperType::CreateDeviceObjectCreateDesc("CommandPool 1"), 0, GetDesc().queueFamilyIndex, myQueue}),
	  })
{
#if (SPEEDO_PROFILING_LEVEL > 0)
	// a ring of 4096 timestamps, as Vulkan's backend has (a frame writes about 40). no setup command buffer: the first
	// timestamp is the queue's clock (GetClockCalibration)
	if (GetDesc().supportsProfiling)
		myProfiler = std::make_unique<GpuProfiler<kD3D12>>(
			QueryPool<kD3D12>(QueryPoolCreateDesc<kD3D12>{
				SuperType::CreateDeviceObjectCreateDesc(std::format("{} Profiler", GetDesc().name)), 4096}),
			myQueue,
			nullptr,
			GetDesc().name);
#endif
}

template <>
Queue<kD3D12>::Queue(QueueCreateDesc<kD3D12>&& queueDesc)
	: Queue(
		std::forward<QueueCreateDesc<kD3D12>>(queueDesc),
		// read from queueDesc, not GetDesc(): this runs before the delegated constructor has initialized the base
		[&queueDesc]
		{
			const D3D12_COMMAND_QUEUE_DESC desc{
				.Type = d3d12::ToD3D12(queueDesc.queueFamilyIndex),
				.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL,
				.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE,
				.NodeMask = 0};
			ID3D12CommandQueue* queue = nullptr;
			d3d12::Check(queueDesc.device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)), "CreateCommandQueue");
			d3d12::SetName(queue, queueDesc.name);
			return queue;
		}())
{}

template <>
void Queue<kD3D12>::Swap(Queue& other) noexcept
{
	DeviceObject<Queue<kD3D12>>::Swap(other);
	std::swap(myQueue, other.myQueue);
	std::swap(myPools, other.myPools);
	std::swap(myPendingSubmits, other.myPendingSubmits);
	std::swap(myScratchMemory, other.myScratchMemory);
	std::swap(myTimelineCallbacks, other.myTimelineCallbacks);
	std::swap(myProfiler, other.myProfiler);
}

template <>
Queue<kD3D12>::Queue(Queue<kD3D12>&& other) noexcept
{
	Swap(other);
}

template <>
Queue<kD3D12>& Queue<kD3D12>::operator=(Queue<kD3D12>&& other) noexcept
{
	Swap(other);
	return *this;
}

template <>
Queue<kD3D12>::~Queue()
{
	ZoneScopedN("Queue::~Queue()");

	myProfiler.reset();
	ASSERT(myTimelineCallbacks.size_approx() == 0);
	if (!IsValid())
		return;

	// the pools first: their command buffers are this queue's
	myPools = {};
	myQueue->Release();
}

template <>
QueueHostSyncInfo<kD3D12> Queue<kD3D12>::Submit()
{
	ZoneScopedN("Queue::Submit");

	uint64_t maxTimelineValue = 0;
	std::vector<ID3D12CommandList*> lists;
	std::vector<d3d12::CommandPoolObject*> pools;
	for (auto& submit : myPendingSubmits)
	{
		// binary semaphores are null (the swapchain's acquires and presents, which the queue orders) and skipped, as are
		// waits for 0 (always met)
		for (size_t waitIt = 0; waitIt < submit.waitSemaphores.size(); waitIt++)
			if (submit.waitSemaphores[waitIt] != nullptr && submit.waitSemaphoreValues[waitIt] > 0)
				d3d12::Check(myQueue->Wait(submit.waitSemaphores[waitIt], submit.waitSemaphoreValues[waitIt]), "ID3D12CommandQueue::Wait");

		lists.clear();
		pools.clear();
		for (auto* object : submit.commandBuffers)
		{
			ENSUREF(!object->recording, "a command buffer submitted while it's recording (CommandBuffer::End)");
			lists.push_back(object->list);
			if (std::ranges::find(pools, object->pool) == pools.end())
				pools.push_back(object->pool);
		}
		if (!lists.empty())
			myQueue->ExecuteCommandLists(static_cast<UINT>(lists.size()), lists.data());
		// the pools' fences, which their resets wait for
		for (auto* pool : pools)
			d3d12::Check(myQueue->Signal(pool->done, ++pool->submitted), "ID3D12CommandQueue::Signal (a command pool's)");

		for (size_t signalIt = 0; signalIt < submit.signalSemaphores.size(); signalIt++)
			if (submit.signalSemaphores[signalIt] != nullptr)
				d3d12::Check(
					myQueue->Signal(submit.signalSemaphores[signalIt], submit.signalSemaphoreValues[signalIt]), "ID3D12CommandQueue::Signal");

		// the crash report's log of submits (the D3D12 backend has no hook of its own in it)
		CrashSubmit(reinterpret_cast<uint64_t>(myQueue), static_cast<uint32_t>(submit.commandBuffers.size()), submit.timelineValue);

		if (!submit.callbacks.empty())
			myTimelineCallbacks.enqueue({std::move(submit.callbacks), submit.timelineValue});

		maxTimelineValue = std::max(maxTimelineValue, submit.timelineValue);
	}
	myPendingSubmits.clear();

	return QueueHostSyncInfo<kD3D12>{.fences = {}, .maxTimelineValue = maxTimelineValue};
}

template <>
QueueHostSyncInfo<kD3D12> Queue<kD3D12>::Present(PresentResult* /*presentResult*/)
{
	ENSUREF(false, "the D3D12 backend has no swapchain yet (docs/backends-plan.md, step 5)");
	return {};
}

template <>
void Queue<kD3D12>::WaitIdle() const
{
	ZoneScopedN("Queue::WaitIdle");

	// a fence of its own signaled last, waited for: the queue runs in order
	ID3D12Device* device = nullptr;
	d3d12::Check(myQueue->GetDevice(IID_PPV_ARGS(&device)), "ID3D12CommandQueue::GetDevice");
	ID3D12Fence* fence = nullptr;
	d3d12::Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
	d3d12::Check(myQueue->Signal(fence, 1), "ID3D12CommandQueue::Signal");
	d3d12::Check(fence->SetEventOnCompletion(1, nullptr), "ID3D12Fence::SetEventOnCompletion"); // a null event: blocks
	fence->Release();
	device->Release();
}

} // namespace rhi
