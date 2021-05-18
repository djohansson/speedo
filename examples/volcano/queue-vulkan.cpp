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

//

template <>
QueueContext<Vk>::QueueContext(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    std::tuple<QueueContextCreateDesc<Vk>, QueueHandle<Vk>>&& descAndHandle)
: DeviceObject(
    deviceContext,
    {"_Queue"},
    1,
    VK_OBJECT_TYPE_QUEUE,
    reinterpret_cast<uint64_t*>(&std::get<1>(descAndHandle)))
, myDesc(std::forward<QueueContextCreateDesc<Vk>>(std::get<0>(descAndHandle)))
, myQueue(std::get<1>(descAndHandle))
{
    //if constexpr (PROFILING_ENABLED)
#if PROFILING_ENABLED
    {
        if (auto cmd = myDesc.tracingEnableInitCmd.value_or(CommandBufferHandle<Vk>{VK_NULL_HANDLE}))
        {
            myUserData = queue::UserData{
                tracy::CreateVkContext(
                    deviceContext->getPhysicalDevice(),
                    deviceContext->getDevice(),
                    myQueue,
                    cmd,
                    nullptr,
                    nullptr)};
        }
    }
#endif
}

template <>
QueueContext<Vk>::QueueContext(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    QueueContextCreateDesc<Vk>&& desc)
: QueueContext(
    deviceContext,
    std::make_tuple(
        std::forward<QueueContextCreateDesc<Vk>>(desc),
        [device = deviceContext->getDevice(), &desc]()
        {
            QueueHandle<Vk> queue;
            vkGetDeviceQueue(device, desc.queueFamilyIndex, desc.queueIndex, &queue);
            return queue;
        }()))
{
}

template <>
QueueContext<Vk>::QueueContext(QueueContext<Vk>&& other) noexcept
: DeviceObject(std::forward<QueueContext<Vk>>(other))
, myDesc(std::exchange(other.myDesc, {}))
, myQueue(std::exchange(other.myQueue, {}))
, myPendingSubmits(std::exchange(other.myPendingSubmits, {}))
, myScratchMemory(std::exchange(other.myScratchMemory, {}))
, myFence(std::exchange(other.myFence, {}))
, myUserData(std::exchange(other.myUserData, {}))
{
}

template <>
QueueContext<Vk>::~QueueContext()
{
    //if constexpr (PROFILING_ENABLED)
#if PROFILING_ENABLED
    {
        if (myDesc.tracingEnableInitCmd)
            tracy::DestroyVkContext(std::any_cast<queue::UserData>(&myUserData)->tracyContext);
    }
#endif
}

template <>
QueueContext<Vk>& QueueContext<Vk>::operator=(QueueContext<Vk>&& other) noexcept
{
    DeviceObject::operator=(std::forward<QueueContext<Vk>>(other));
    myDesc = std::exchange(other.myDesc, {});
    myQueue = std::exchange(other.myQueue, {});
    myPendingSubmits = std::exchange(other.myPendingSubmits, {});
    myScratchMemory = std::exchange(other.myScratchMemory, {});
    myFence = std::exchange(other.myFence, {});
    myUserData = std::exchange(other.myUserData, {});
	return *this;
}

template <>
void QueueContext<Vk>::swap(QueueContext& rhs) noexcept
{
    DeviceObject::swap(rhs);
    std::swap(myDesc, rhs.myDesc);
    std::swap(myQueue, rhs.myQueue);
    std::swap(myPendingSubmits, rhs.myPendingSubmits);
    std::swap(myScratchMemory, rhs.myScratchMemory);
    std::swap(myFence, rhs.myFence);
    std::swap(myUserData, rhs.myUserData);
}

template <>
void QueueContext<Vk>::traceCollect(CommandBufferHandle<Vk> cmd)
{
    //if constexpr (PROFILING_ENABLED)
#if PROFILING_ENABLED
    {
        if (myDesc.tracingEnableInitCmd)
            TracyVkCollect(std::any_cast<queue::UserData>(&myUserData)->tracyContext, cmd);
    }
#endif
}

template <>
std::shared_ptr<void> QueueContext<Vk>::internalTrace(CommandBufferHandle<Vk> cmd, const SourceLocationData& srcLoc)
{
    //if constexpr (PROFILING_ENABLED)
#if PROFILING_ENABLED
    {
        static_assert(sizeof(SourceLocationData) == sizeof(tracy::SourceLocationData));
        // static_assert(offsetof(SourceLocationData, name) == offsetof(tracy::SourceLocationData, name));
        // static_assert(offsetof(SourceLocationData, function) == offsetof(tracy::SourceLocationData, function));
        // static_assert(offsetof(SourceLocationData, file) == offsetof(tracy::SourceLocationData, file));
        // static_assert(offsetof(SourceLocationData, line) == offsetof(tracy::SourceLocationData, line));
        // static_assert(offsetof(SourceLocationData, color) == offsetof(tracy::SourceLocationData, color));

        if (myDesc.tracingEnableInitCmd)
        {
            return std::make_shared<tracy::VkCtxScope>(
                tracy::VkCtxScope(
                    std::any_cast<queue::UserData>(&myUserData)->tracyContext,
                    reinterpret_cast<const tracy::SourceLocationData*>(&srcLoc),
                    cmd,
                    true));
        }
    }
#endif

    return {};    
}

template <>
uint64_t QueueContext<Vk>::submit()
{
    ZoneScopedN("QueueContext::submit");

    if (myPendingSubmits.empty())
        return 0;

    myScratchMemory.resize(
        (sizeof(SubmitInfo<Vk>) + sizeof(TimelineSemaphoreSubmitInfo<Vk>)) * myPendingSubmits.size());
    
    auto timelineBegin = reinterpret_cast<TimelineSemaphoreSubmitInfo<Vk>*>(myScratchMemory.data());
    auto timelinePtr = timelineBegin;

    uint64_t maxTimelineValue = 0ull;

    for (const auto& pendingSubmit : myPendingSubmits)
    {
        auto& timelineInfo = *(timelinePtr++);

        timelineInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        timelineInfo.pNext = nullptr;
        timelineInfo.waitSemaphoreValueCount = pendingSubmit.waitSemaphoreValues.size();
        timelineInfo.pWaitSemaphoreValues = pendingSubmit.waitSemaphoreValues.data();
        timelineInfo.signalSemaphoreValueCount = pendingSubmit.signalSemaphoreValues.size();
        timelineInfo.pSignalSemaphoreValues = pendingSubmit.signalSemaphoreValues.data();

        maxTimelineValue = std::max<uint64_t>(maxTimelineValue, pendingSubmit.timelineValue);
    }

    auto submitBegin = reinterpret_cast<SubmitInfo<Vk>*>(timelinePtr);
    auto submitPtr = submitBegin;
    timelinePtr = timelineBegin;

    for (const auto& pendingSubmit : myPendingSubmits)
    {
        auto& submitInfo = *(submitPtr++);

        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.pNext = timelinePtr++;
        submitInfo.waitSemaphoreCount = pendingSubmit.waitSemaphores.size();
        submitInfo.pWaitSemaphores = pendingSubmit.waitSemaphores.data();
        submitInfo.pWaitDstStageMask = pendingSubmit.waitDstStageMasks.data();
        submitInfo.signalSemaphoreCount  = pendingSubmit.signalSemaphores.size();
        submitInfo.pSignalSemaphores = pendingSubmit.signalSemaphores.data();
        submitInfo.commandBufferCount = pendingSubmit.commandBuffers.size();
        submitInfo.pCommandBuffers = pendingSubmit.commandBuffers.data();
    }

    VK_CHECK(vkQueueSubmit(myQueue, myPendingSubmits.size(), submitBegin, myFence));

    myPendingSubmits.clear();

    myLastSubmitTimelineValue = maxTimelineValue;

    return maxTimelineValue;
}

template <>
void QueueContext<Vk>::waitIdle() const
{
    ZoneScopedN("QueueContext::waitIdle");

    VK_CHECK(vkQueueWaitIdle(myQueue));
}

template <>
void QueueContext<Vk>::present()
{
    ZoneScopedN("QueueContext::present");

    if (myPendingPresent.swapchains.empty())
        return;

    PresentInfo<Vk> presentInfo = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    presentInfo.waitSemaphoreCount = myPendingPresent.waitSemaphores.size();
    presentInfo.pWaitSemaphores = myPendingPresent.waitSemaphores.data();
    presentInfo.swapchainCount = myPendingPresent.swapchains.size();
    presentInfo.pSwapchains = myPendingPresent.swapchains.data();
    presentInfo.pImageIndices = myPendingPresent.imageIndices.data();
    presentInfo.pResults = myPendingPresent.results.data();
    
    checkFlipOrPresentResult(vkQueuePresentKHR(myQueue, &presentInfo));

    myPendingPresent = {};
}
