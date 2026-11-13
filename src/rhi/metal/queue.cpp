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
#include <format>
#include <memory>
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
{
#if (SPEEDO_PROFILING_LEVEL > 0)
	// a ring of 4096 timestamps, as Vulkan's backend has (a frame writes about 40): two counter sample buffers of their
	// samples (see QueryPool). no setup command buffer: the first timestamp is the device's (sampleTimestamps)
	if (GetDesc().supportsProfiling)
		myProfiler = std::make_unique<GpuProfiler<kMetal>>(
			QueryPool<kMetal>(QueryPoolCreateDesc<kMetal>{
				SuperType::CreateDeviceObjectCreateDesc(std::format("{} Profiler", GetDesc().name)), 4096}),
			myQueue,
			nullptr,
			GetDesc().name);
#endif
}

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
		// already ordered (one queue runs in order, and its resources are hazard tracked). binary semaphores are null (the
		// swapchain's acquires and presents: the queue orders them too) and skipped
		if (std::ranges::any_of(submit.waitSemaphores, [](auto* semaphore) { return semaphore != nullptr; }))
		{
			MTL::CommandBuffer* waits = myQueue->commandBuffer();
			for (size_t waitIt = 0; waitIt < submit.waitSemaphores.size(); waitIt++)
				if (submit.waitSemaphores[waitIt] != nullptr)
					waits->encodeWait(submit.waitSemaphores[waitIt], submit.waitSemaphoreValues[waitIt]);
			waits->commit();
		}

		auto signal = [&submit](MTL::CommandBuffer* commandBuffer)
		{
			for (size_t signalIt = 0; signalIt < submit.signalSemaphores.size(); signalIt++)
				if (submit.signalSemaphores[signalIt] != nullptr)
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
QueueHostSyncInfo<kMetal> Queue<kMetal>::Present(PresentResult* presentResult)
{
	ZoneScopedN("Queue::Present");

	// each swapchain's frame's image copied into its layer's next drawable (waiting for one, as the drawables pace the
	// frames) and presented, in a command buffer committed after the frame's work (the queue orders them: the present's
	// wait semaphores are the swapchain's binary ones, null). no drawable within the layer's second: suboptimal, the frame
	// dropped and the swapchain made again
	const auto swapchainCount = myPendingPresent.swapchains.size();
	ENSURE(swapchainCount > 0 && myPendingPresent.imageIndices.size() == swapchainCount);
	auto result = PresentResult::kSuccess;
	NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
	for (size_t swapchainIt = 0; swapchainIt < swapchainCount; swapchainIt++)
	{
		auto* swapchain = myPendingPresent.swapchains[swapchainIt];
		auto* image = swapchain->textures.at(myPendingPresent.imageIndices[swapchainIt]);
		CA::MetalDrawable* drawable = nullptr;
		{
			ZoneScopedN("Queue::Present::nextDrawable");
			drawable = swapchain->layer->nextDrawable();
		}
		if (drawable == nullptr)
		{
			result = PresentResult::kSuboptimal;
			continue;
		}
		MTL::CommandBuffer* commandBuffer = myQueue->commandBuffer();
		auto* blit = commandBuffer->blitCommandEncoder();
		blit->copyFromTexture(image, drawable->texture());
		blit->endEncoding();
		commandBuffer->presentDrawable(drawable);
		commandBuffer->commit();
	}
	pool->release();
	myPendingPresent = {};
	if (presentResult != nullptr)
		*presentResult = result;
	return {};
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
