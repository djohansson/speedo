// the Metal backend's queue: an MTL::CommandQueue, and its submissions on the timelines (shared events, see
// docs/backends-plan.md): a signal goes on a submission's last command buffer before it is committed, a wait in a
// command buffer committed just before the submission's
#include <rhi/metal/backend.h>

#include <rhi/device.h>
#include <rhi/queue.h>
#include <rhi/rhi.h>

#include <core/assert.h>
#include <core/profiling.h>

#include <algorithm>
#include <string>
#include <utility>

namespace rhi
{

IMPLEMENT_OBJECT_GETINSTANCE(Queue<kMetal>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(Queue<kMetal>);

template <>
Queue<kMetal>::Queue(CreateDescType&& desc, QueueHandle<kMetal>&& handle)
	: DeviceObject<Queue<kMetal>>(std::forward<CreateDescType>(desc))
	, myQueue(std::forward<QueueHandle<kMetal>>(handle))
	, myPools({
		  CommandPool<kMetal>(CommandPoolCreateDesc<kMetal>{
			  SuperType::CreateDeviceObjectCreateDesc("CommandPool 0"), 0, GetDesc().queueFamilyIndex, myQueue}),
		  CommandPool<kMetal>(CommandPoolCreateDesc<kMetal>{
			  SuperType::CreateDeviceObjectCreateDesc("CommandPool 1"), 0, GetDesc().queueFamilyIndex, myQueue}),
	  })
{}

template <>
Queue<kMetal>::Queue(QueueCreateDesc<kMetal>&& queueDesc)
	: Queue(
		std::forward<QueueCreateDesc<kMetal>>(queueDesc),
		// read from queueDesc, not GetDesc(): this runs before the delegated constructor has initialized the base
		[&queueDesc]
		{
			MTL::CommandQueue* queue = queueDesc.device->newCommandQueue();
			ENSUREF(queue != nullptr, "Failed to create a Metal command queue");
			queue->setLabel(NS::String::string(queueDesc.name.c_str(), NS::UTF8StringEncoding));
			return queue;
		}())
{}

template <>
void Queue<kMetal>::Swap(Queue& other) noexcept
{
	DeviceObject<Queue<kMetal>>::Swap(other);
	std::swap(myQueue, other.myQueue);
	std::swap(myPools, other.myPools);
	std::swap(myPendingSubmits, other.myPendingSubmits);
	std::swap(myScratchMemory, other.myScratchMemory);
	std::swap(myTimelineCallbacks, other.myTimelineCallbacks);
	std::swap(myProfiler, other.myProfiler);
}

template <>
Queue<kMetal>::Queue(Queue<kMetal>&& other) noexcept
{
	Swap(other);
}

template <>
Queue<kMetal>& Queue<kMetal>::operator=(Queue<kMetal>&& other) noexcept
{
	Swap(other);
	return *this;
}

template <>
Queue<kMetal>::~Queue()
{
	ZoneScopedN("Queue::~Queue()");

	myProfiler.reset();
	ASSERT(myTimelineCallbacks.size_approx() == 0);
	if (!IsValid())
		return;

	// the pools first: their command buffers are this queue's
	myPools = {};
	myQueue->release();
}

template <>
QueueHostSyncInfo<kMetal> Queue<kMetal>::Submit()
{
	ZoneScopedN("Queue::Submit");

	uint64_t maxTimelineValue = 0;
	NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
	for (auto& submit : myPendingSubmits)
	{
		// the waits, in a command buffer of their own committed before the work. on the queue's own timeline they are
		// already ordered (one queue runs in order, and its resources are hazard tracked)
		if (!submit.waitSemaphores.empty())
		{
			MTL::CommandBuffer* waits = myQueue->commandBuffer();
			for (size_t waitIt = 0; waitIt < submit.waitSemaphores.size(); waitIt++)
				waits->encodeWait(submit.waitSemaphores[waitIt], submit.waitSemaphoreValues[waitIt]);
			waits->commit();
		}

		auto signal = [&submit](MTL::CommandBuffer* commandBuffer)
		{
			for (size_t signalIt = 0; signalIt < submit.signalSemaphores.size(); signalIt++)
				commandBuffer->encodeSignalEvent(submit.signalSemaphores[signalIt], submit.signalSemaphoreValues[signalIt]);
		};
		for (size_t commandIt = 0; commandIt < submit.commandBuffers.size(); commandIt++)
		{
			auto* object = submit.commandBuffers[commandIt];
			object->EndEncoders();
			if (commandIt + 1 == submit.commandBuffers.size())
				signal(object->commandBuffer);
			object->commandBuffer->commit();
		}
		if (submit.commandBuffers.empty() && !submit.signalSemaphores.empty())
		{
			MTL::CommandBuffer* signals = myQueue->commandBuffer();
			signal(signals);
			signals->commit();
		}

		if (!submit.callbacks.empty())
			myTimelineCallbacks.enqueue({std::move(submit.callbacks), submit.timelineValue});

		maxTimelineValue = std::max(maxTimelineValue, submit.timelineValue);
	}
	pool->release();
	myPendingSubmits.clear();

	return QueueHostSyncInfo<kMetal>{.fences = {}, .maxTimelineValue = maxTimelineValue};
}

template <>
void Queue<kMetal>::WaitIdle() const
{
	ZoneScopedN("Queue::WaitIdle");

	// an empty command buffer committed last, waited for: the queue runs in order
	NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
	MTL::CommandBuffer* last = myQueue->commandBuffer();
	last->commit();
	last->waitUntilCompleted();
	pool->release();
}

} // namespace rhi
