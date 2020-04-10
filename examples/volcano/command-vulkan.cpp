#include "command.h"

template <>
thread_local std::vector<std::byte> CommandContext<GraphicsBackend::Vulkan>::threadScratchMemory(1024);

template <>
bool CommandContext<GraphicsBackend::Vulkan>::isComplete() const
{
    uint64_t value;
    CHECK_VK(vkGetSemaphoreCounterValue(myDesc.device, myDesc.timelineSemaphore, &value));

    return (value >= myLastSubmitTimelineValue);
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
void CommandContext<GraphicsBackend::Vulkan>::submit(
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

    auto timelineValue = myDesc.timelineValue->fetch_add(signalSemaphoreCount, std::memory_order_relaxed);

    auto writePtr = threadScratchMemory.data();
    
    auto waitSemaphoresBegin = reinterpret_cast<SemaphoreHandle<GraphicsBackend::Vulkan>*>(writePtr);
    {
        auto waitSemaphoresPtr = waitSemaphoresBegin;
        for (uint32_t i = 0; i < submitInfo.waitSemaphoreCount; i++)
            *waitSemaphoresPtr++ = submitInfo.waitSemaphores[i];
        *waitSemaphoresPtr++ = myDesc.timelineSemaphore;

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
            *waitSemaphoreValuesPtr++ = timelineValue;
        *waitSemaphoreValuesPtr++ = timelineValue;

        writePtr = reinterpret_cast<std::byte*>(waitSemaphoreValuesPtr);
    }
    
    auto signalSemaphoresBegin = reinterpret_cast<SemaphoreHandle<GraphicsBackend::Vulkan>*>(writePtr);
    {
        auto signalSemaphoresPtr = signalSemaphoresBegin;
        for (uint32_t i = 0; i < submitInfo.signalSemaphoreCount; i++)
            *signalSemaphoresPtr++ = submitInfo.signalSemaphores[i];
        *signalSemaphoresPtr++ = myDesc.timelineSemaphore;

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

    myLastSubmitTimelineValue = timelineValue;

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

    CHECK_VK(vkQueueSubmit(myDesc.queue, 1, &vkSubmitInfo, submitInfo.signalFence));
}

template <>
void CommandContext<GraphicsBackend::Vulkan>::end() const
{
    CHECK_VK(vkEndCommandBuffer(myCommandBuffer));
}

template <>
void CommandContext<GraphicsBackend::Vulkan>::sync() const
{
    if (!isComplete())
    {
        VkSemaphoreWaitInfo waitInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
        waitInfo.flags = 0;
        waitInfo.semaphoreCount = 1;
        waitInfo.pSemaphores = &myDesc.timelineSemaphore;
        waitInfo.pValues = &myLastSubmitTimelineValue;

        CHECK_VK(vkWaitSemaphores(myDesc.device, &waitInfo, UINT64_MAX));
    }
}

template <>
void CommandContext<GraphicsBackend::Vulkan>::free()
{
    if (myCommandBuffer)
    {
        assert(isComplete());

        vkFreeCommandBuffers(myDesc.device, myDesc.commandPool, 1, &myCommandBuffer);
        myCommandBuffer = 0;
    }
}

template <>
CommandContext<GraphicsBackend::Vulkan>::CommandContext(CommandCreateDesc<GraphicsBackend::Vulkan>&& desc)
: myDesc(std::move(desc))
{
    VkCommandBufferAllocateInfo cmdInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cmdInfo.commandPool = myDesc.commandPool;
    cmdInfo.level = static_cast<VkCommandBufferLevel>(myDesc.commandBufferLevel);
    cmdInfo.commandBufferCount = 1;
    CHECK_VK(vkAllocateCommandBuffers(myDesc.device, &cmdInfo, &myCommandBuffer));
}

template <>
CommandContext<GraphicsBackend::Vulkan>::~CommandContext()
{
    free();
}
