#include "command.h"

template <>
thread_local std::vector<std::byte> CommandContext<GraphicsBackend::Vulkan>::threadScratchMemory(1024);

template <>
bool CommandContext<GraphicsBackend::Vulkan>::hasReached(uint64_t timelineValue) const
{
    uint64_t value;
    CHECK_VK(vkGetSemaphoreCounterValue(myCommandDesc.deviceContext->getDevice(), myCommandDesc.timelineSemaphore, &value));

    return (value >= timelineValue);
}

template <>
void CommandContext<GraphicsBackend::Vulkan>::begin(const CommandBufferBeginInfo<GraphicsBackend::Vulkan>* beginInfo) const
{
    VkCommandBufferBeginInfo defaultBeginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    defaultBeginInfo.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    CHECK_VK(vkResetCommandBuffer(myCommandBuffer, 0));
    CHECK_VK(vkBeginCommandBuffer(myCommandBuffer, beginInfo ? beginInfo : &defaultBeginInfo));
}

template <>
uint64_t CommandContext<GraphicsBackend::Vulkan>::submit(
    const CommandSubmitInfo<GraphicsBackend::Vulkan>& submitInfo)
{
    const auto waitSemaphoreCount = submitInfo.waitSemaphoreCount + 1;
    const auto signalSemaphoreCount = submitInfo.signalSemaphoreCount + 1;
    
    threadScratchMemory.resize(
        sizeof(uint64_t) * waitSemaphoreCount +
        sizeof(SemaphoreHandle<GraphicsBackend::Vulkan>) * waitSemaphoreCount +
        sizeof(Flags<GraphicsBackend::Vulkan>) * waitSemaphoreCount +
        sizeof(uint64_t) * signalSemaphoreCount + 
        sizeof(SemaphoreHandle<GraphicsBackend::Vulkan>) * signalSemaphoreCount);

    auto writePtr = threadScratchMemory.data();
    
    auto waitSemaphoresBegin = reinterpret_cast<SemaphoreHandle<GraphicsBackend::Vulkan>*>(writePtr);
    {
        auto waitSemaphoresPtr = waitSemaphoresBegin;
        for (uint32_t i = 0; i < submitInfo.waitSemaphoreCount; i++)
            *waitSemaphoresPtr++ = submitInfo.waitSemaphores[i];
        *waitSemaphoresPtr++ = myCommandDesc.timelineSemaphore;

        writePtr = reinterpret_cast<std::byte*>(waitSemaphoresPtr);
    }

    auto waitDstStageMasksBegin = reinterpret_cast<Flags<GraphicsBackend::Vulkan>*>(writePtr);
    {
        auto waitDstStageMasksPtr = waitDstStageMasksBegin;
        for (uint32_t i = 0; i < submitInfo.waitSemaphoreCount; i++)
            *waitDstStageMasksPtr++ = submitInfo.waitDstStageMasks[i];
        *waitDstStageMasksPtr++ = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;

        writePtr = reinterpret_cast<std::byte*>(waitDstStageMasksPtr);
    }

    auto waitSemaphoreValuesBegin = reinterpret_cast<uint64_t*>(writePtr);
    {
        auto waitSemaphoreValuesPtr = waitSemaphoreValuesBegin;
        for (uint32_t i = 0; i < submitInfo.waitSemaphoreCount; i++)
            *waitSemaphoreValuesPtr++ = myLastSubmitTimelineValue;
        *waitSemaphoreValuesPtr++ = myLastSubmitTimelineValue;

        writePtr = reinterpret_cast<std::byte*>(waitSemaphoreValuesPtr);
    }

    auto timelineValue = myCommandDesc.timelineValue->fetch_add(signalSemaphoreCount, std::memory_order_relaxed);
    
    auto signalSemaphoresBegin = reinterpret_cast<SemaphoreHandle<GraphicsBackend::Vulkan>*>(writePtr);
    {
        auto signalSemaphoresPtr = signalSemaphoresBegin;
        for (uint32_t i = 0; i < submitInfo.signalSemaphoreCount; i++)
            *signalSemaphoresPtr++ = submitInfo.signalSemaphores[i];
        *signalSemaphoresPtr++ = myCommandDesc.timelineSemaphore;

        writePtr = reinterpret_cast<std::byte*>(signalSemaphoresPtr);
    }

    auto signalSemaphoreValuesBegin = reinterpret_cast<uint64_t*>(writePtr);
    {
        auto signalSemaphoreValuesPtr = signalSemaphoreValuesBegin;
        for (uint32_t i = 0; i < submitInfo.signalSemaphoreCount; i++)
            *signalSemaphoreValuesPtr++ = ++timelineValue;
        *signalSemaphoreValuesPtr++ = ++timelineValue;

        writePtr = reinterpret_cast<std::byte*>(signalSemaphoreValuesPtr);
    }

    VkTimelineSemaphoreSubmitInfo timelineInfo = { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
    timelineInfo.pNext = nullptr;
    timelineInfo.waitSemaphoreValueCount = waitSemaphoreCount;
    timelineInfo.pWaitSemaphoreValues = waitSemaphoreValuesBegin;
    timelineInfo.signalSemaphoreValueCount = signalSemaphoreCount;
    timelineInfo.pSignalSemaphoreValues = signalSemaphoreValuesBegin;

    VkSubmitInfo vkSubmitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    vkSubmitInfo.pNext = &timelineInfo;
    vkSubmitInfo.waitSemaphoreCount = waitSemaphoreCount;
    vkSubmitInfo.pWaitSemaphores = waitSemaphoresBegin;
    vkSubmitInfo.pWaitDstStageMask = waitDstStageMasksBegin;
    vkSubmitInfo.signalSemaphoreCount  = signalSemaphoreCount;
    vkSubmitInfo.pSignalSemaphores = signalSemaphoresBegin;
    vkSubmitInfo.commandBufferCount = 1;
    vkSubmitInfo.pCommandBuffers = &myCommandBuffer;

    CHECK_VK(vkQueueSubmit(myCommandDesc.deviceContext->getSelectedQueue(), 1, &vkSubmitInfo, submitInfo.signalFence));

    myLastSubmitTimelineValue = timelineValue;

    return timelineValue;
}

template <>
void CommandContext<GraphicsBackend::Vulkan>::end() const
{
    CHECK_VK(vkEndCommandBuffer(myCommandBuffer));
}

template <>
void CommandContext<GraphicsBackend::Vulkan>::sync(std::optional<uint64_t> timelineValue)
{
    uint64_t value = timelineValue.value_or(myLastSubmitTimelineValue);

    if (!hasReached(value))
    {
        VkSemaphoreWaitInfo waitInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
        waitInfo.flags = 0;
        waitInfo.semaphoreCount = 1;
        waitInfo.pSemaphores = &myCommandDesc.timelineSemaphore;
        waitInfo.pValues = &value;

        CHECK_VK(vkWaitSemaphores(myCommandDesc.deviceContext->getDevice(), &waitInfo, UINT64_MAX));
    }

    for (auto& callback : mySyncCallbacks)
        callback.first();
    
    mySyncCallbacks.clear();
}

template <>
void CommandContext<GraphicsBackend::Vulkan>::free()
{
    if (myCommandBuffer)
    {
        assert(hasReached(myLastSubmitTimelineValue));

        vkFreeCommandBuffers(myCommandDesc.deviceContext->getDevice(), myCommandDesc.commandPool, 1, &myCommandBuffer);
        myCommandBuffer = 0;
    }
}

template <>
CommandContext<GraphicsBackend::Vulkan>::CommandContext(CommandDesc<GraphicsBackend::Vulkan>&& desc)
: myCommandDesc(std::move(desc))
{
    VkCommandBufferAllocateInfo cmdInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cmdInfo.commandPool = myCommandDesc.commandPool;
    cmdInfo.level = static_cast<VkCommandBufferLevel>(myCommandDesc.commandBufferLevel);
    cmdInfo.commandBufferCount = 1;
    CHECK_VK(vkAllocateCommandBuffers(myCommandDesc.deviceContext->getDevice(), &cmdInfo, &myCommandBuffer));
}

template <>
CommandContext<GraphicsBackend::Vulkan>::~CommandContext()
{
    free();
}
