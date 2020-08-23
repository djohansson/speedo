#include "queue.h"
#include "vk-utils.h"

template <>
Queue<Vk>::Queue(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    QueueCreateDesc<Vk>&& desc)
: DeviceResource<Vk>(
    deviceContext,
    {"_Queue"},
    1,
    VK_OBJECT_TYPE_QUEUE,
    reinterpret_cast<uint64_t*>(&desc.queue))
, myDesc(std::move(desc))
{
}

template <>
Queue<Vk>::Queue(Queue<Vk>&& other)
: DeviceResource<Vk>(std::move(other))
, myDesc(std::exchange(other.myDesc, {}))
, myPendingSubmits(std::exchange(other.myPendingSubmits, {}))
, myScratchMemory(std::exchange(other.myScratchMemory, {}))
, myFence(std::exchange(other.myFence, {}))
{
}

template <>
Queue<Vk>& Queue<Vk>::operator=(Queue<Vk>&& other)
{
	DeviceResource<Vk>::operator=(std::move(other));
	myDesc = std::exchange(other.myDesc, {});
    myPendingSubmits = std::exchange(other.myPendingSubmits, {});
    myScratchMemory = std::exchange(other.myScratchMemory, {});
    myFence = std::exchange(other.myFence, {});
	return *this;
}

template <>
uint64_t Queue<Vk>::submit()
{
    ZoneScopedN("Queue::submit");

    if (myPendingSubmits.empty())
        return 0;

    myScratchMemory.resize(
        (sizeof(SubmitInfo<Vk>) + sizeof(TimelineSemaphoreSubmitInfo<Vk>)) * myPendingSubmits.size());
    
    auto timelineBegin = reinterpret_cast<TimelineSemaphoreSubmitInfo<Vk>*>(myScratchMemory.data());
    auto timelinePtr = timelineBegin;

    uint64_t maxTimelineValue = 0;

    for (const auto& pendingSubmit : myPendingSubmits)
    {
        auto& timelineInfo = *(timelinePtr++);

        timelineInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        timelineInfo.pNext = nullptr;
        timelineInfo.waitSemaphoreValueCount = pendingSubmit.syncInfo.waitSemaphoreValues.size();
        timelineInfo.pWaitSemaphoreValues = pendingSubmit.syncInfo.waitSemaphoreValues.data();
        timelineInfo.signalSemaphoreValueCount = pendingSubmit.syncInfo.signalSemaphoreValues.size();
        timelineInfo.pSignalSemaphoreValues = pendingSubmit.syncInfo.signalSemaphoreValues.data();

        maxTimelineValue = std::max<uint64_t>(maxTimelineValue, pendingSubmit.maxTimelineValue);
    }

    auto submitBegin = reinterpret_cast<SubmitInfo<Vk>*>(timelinePtr);
    auto submitPtr = submitBegin;
    timelinePtr = timelineBegin;

    for (const auto& pendingSubmit : myPendingSubmits)
    {
        auto& submitInfo = *(submitPtr++);

        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.pNext = timelinePtr++;
        submitInfo.waitSemaphoreCount = pendingSubmit.syncInfo.waitSemaphores.size();
        submitInfo.pWaitSemaphores = pendingSubmit.syncInfo.waitSemaphores.data();
        submitInfo.pWaitDstStageMask = pendingSubmit.syncInfo.waitDstStageMasks.data();
        submitInfo.signalSemaphoreCount  = pendingSubmit.syncInfo.signalSemaphores.size();
        submitInfo.pSignalSemaphores = pendingSubmit.syncInfo.signalSemaphores.data();
        submitInfo.commandBufferCount = pendingSubmit.commandBuffers.size();
        submitInfo.pCommandBuffers = pendingSubmit.commandBuffers.data();
    }

    VK_CHECK(vkQueueSubmit(myDesc.queue, myPendingSubmits.size(), submitBegin, myFence));
    
    myPendingSubmits.clear();

    return maxTimelineValue;
}

template <>
void Queue<Vk>::waitIdle() const
{
    ZoneScopedN("Queue::waitIdle");

    VK_CHECK(vkQueueWaitIdle(myDesc.queue));
}
