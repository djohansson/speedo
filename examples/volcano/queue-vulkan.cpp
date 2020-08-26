#include "queue.h"
#include "vk-utils.h"

#include <TracyC.h>
#include <TracyVulkan.hpp>

namespace queue
{

struct UserData
{
    TracyVkCtx tracyContext = {};
};

}

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
    if (myDesc.tracingEnabled)
    {
        auto physicalDevice = deviceContext->getPhysicalDevice();
        auto device = deviceContext->getDevice();
        auto pool = deviceContext->getQueueFamilies(deviceContext->getGraphicsQueueFamilyIndex()).commandPools.front();

        VkCommandBuffer cmd;
        VkCommandBufferAllocateInfo cmdInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cmdInfo.commandPool = pool;
        cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmdInfo.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(device, &cmdInfo, &cmd));

        auto tracyContext = TracyVkContext(physicalDevice, device, myDesc.queue, cmd);

        vkFreeCommandBuffers(device, pool, 1, &cmd);

        myUserData = queue::UserData{tracyContext};
    }
}

template <>
Queue<Vk>::Queue(Queue<Vk>&& other)
: DeviceResource<Vk>(std::move(other))
, myDesc(std::exchange(other.myDesc, {}))
, myPendingSubmits(std::exchange(other.myPendingSubmits, {}))
, myScratchMemory(std::exchange(other.myScratchMemory, {}))
, myFence(std::exchange(other.myFence, {}))
, myUserData(std::exchange(other.myUserData, {}))
{
}

template <>
Queue<Vk>::~Queue()
{
    if (myDesc.tracingEnabled)
        TracyVkDestroy(std::any_cast<queue::UserData>(&myUserData)->tracyContext);
}

template <>
Queue<Vk>& Queue<Vk>::operator=(Queue<Vk>&& other)
{
	DeviceResource<Vk>::operator=(std::move(other));
	myDesc = std::exchange(other.myDesc, {});
    myPendingSubmits = std::exchange(other.myPendingSubmits, {});
    myScratchMemory = std::exchange(other.myScratchMemory, {});
    myFence = std::exchange(other.myFence, {});
    myUserData = std::exchange(other.myUserData, {});
	return *this;
}

template <>
void Queue<Vk>::collect(CommandBufferHandle<Vk> cmd)
{
    if (myDesc.tracingEnabled)
        TracyVkCollect(std::any_cast<queue::UserData>(&myUserData)->tracyContext, cmd);
}

template <>
std::shared_ptr<void> Queue<Vk>::internalTrace(CommandBufferHandle<Vk> cmd, const SourceLocationData& srcLoc)
{
    static_assert(sizeof(SourceLocationData) == sizeof(tracy::SourceLocationData));
    static_assert(offsetof(SourceLocationData, name) == offsetof(tracy::SourceLocationData, name));
    static_assert(offsetof(SourceLocationData, function) == offsetof(tracy::SourceLocationData, function));
    static_assert(offsetof(SourceLocationData, file) == offsetof(tracy::SourceLocationData, file));
    static_assert(offsetof(SourceLocationData, line) == offsetof(tracy::SourceLocationData, line));
    static_assert(offsetof(SourceLocationData, color) == offsetof(tracy::SourceLocationData, color));

    auto scope = tracy::VkCtxScope(
        std::any_cast<queue::UserData>(&myUserData)->tracyContext,
        reinterpret_cast<const tracy::SourceLocationData*>(&srcLoc),
        cmd,
        true);

    return std::make_shared<tracy::VkCtxScope>(std::move(scope));
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
