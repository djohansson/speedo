#include "command.h"


template <>
thread_local std::vector<std::byte> CommandContext<GraphicsBackend::Vulkan>::threadScratchMemory(1024);

template <>
bool CommandContext<GraphicsBackend::Vulkan>::hasReached(uint64_t timelineValue) const
{
    uint64_t value;
    CHECK_VK(vkGetSemaphoreCounterValue(myCommandContextDesc.deviceContext->getDevice(), myCommandContextDesc.timelineSemaphore, &value));

    return (value >= timelineValue);
}

template <>
void CommandContext<GraphicsBackend::Vulkan>::enqueueAllPendingToSubmitted(uint64_t timelineValue)
{
    mySubmittedCommands.splice(mySubmittedCommands.end(), std::move(myPendingCommands));

    addSyncCallback([this]
    {
        auto freeBeginIt = mySubmittedCommands.begin();
        auto freeEndIt = freeBeginIt;
        while (freeEndIt != mySubmittedCommands.end() && hasReached(freeEndIt->timelineValue()))
            (freeEndIt++)->index() = 0;
        
        myFreeCommands.splice(myFreeCommands.end(), std::move(mySubmittedCommands), freeBeginIt, freeEndIt);

    }, timelineValue);

    myLastSubmitTimelineValue = timelineValue;
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
        sizeof(SemaphoreHandle<GraphicsBackend::Vulkan>) * signalSemaphoreCount +
        sizeof(VkTimelineSemaphoreSubmitInfo) * myPendingCommands.size() + 
        sizeof(VkSubmitInfo) * myPendingCommands.size());

    auto writePtr = threadScratchMemory.data();

    auto waitSemaphoresBegin = reinterpret_cast<SemaphoreHandle<GraphicsBackend::Vulkan>*>(writePtr);
    {
        auto waitSemaphoresPtr = waitSemaphoresBegin;
        for (uint32_t i = 0; i < submitInfo.waitSemaphoreCount; i++)
            *waitSemaphoresPtr++ = submitInfo.waitSemaphores[i];
        *waitSemaphoresPtr++ = myCommandContextDesc.timelineSemaphore;

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
            *waitSemaphoreValuesPtr++ = myLastSubmitTimelineValue.value_or(0);
        *waitSemaphoreValuesPtr++ = myLastSubmitTimelineValue.value_or(0);

        writePtr = reinterpret_cast<std::byte*>(waitSemaphoreValuesPtr);
    }

    auto timelineValue = myCommandContextDesc.timelineValue->fetch_add(signalSemaphoreCount, std::memory_order_relaxed);
    
    auto signalSemaphoresBegin = reinterpret_cast<SemaphoreHandle<GraphicsBackend::Vulkan>*>(writePtr);
    {
        auto signalSemaphoresPtr = signalSemaphoresBegin;
        for (uint32_t i = 0; i < submitInfo.signalSemaphoreCount; i++)
            *signalSemaphoresPtr++ = submitInfo.signalSemaphores[i];
        *signalSemaphoresPtr++ = myCommandContextDesc.timelineSemaphore;

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

    auto timelineSemaphoreSubmitInfoBegin = reinterpret_cast<VkTimelineSemaphoreSubmitInfo*>(writePtr);
    auto timelineSemaphoreSubmitInfoPtr = timelineSemaphoreSubmitInfoBegin;
    auto submitInfoBegin = reinterpret_cast<VkSubmitInfo*>(writePtr + sizeof(VkTimelineSemaphoreSubmitInfo) * myPendingCommands.size());
    auto submitInfoPtr = submitInfoBegin;

    for (auto& cmd : myPendingCommands)
    {
        VkTimelineSemaphoreSubmitInfo& timelineInfo = *timelineSemaphoreSubmitInfoPtr++;
        timelineInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        timelineInfo.pNext = nullptr;
        timelineInfo.waitSemaphoreValueCount = waitSemaphoreCount;
        timelineInfo.pWaitSemaphoreValues = waitSemaphoreValuesBegin;
        timelineInfo.signalSemaphoreValueCount = signalSemaphoreCount;
        timelineInfo.pSignalSemaphoreValues = signalSemaphoreValuesBegin;
        
        VkSubmitInfo& vkSubmitInfo = *submitInfoPtr++;
        vkSubmitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        vkSubmitInfo.pNext = &timelineInfo;
        vkSubmitInfo.waitSemaphoreCount = waitSemaphoreCount;
        vkSubmitInfo.pWaitSemaphores = waitSemaphoresBegin;
        vkSubmitInfo.pWaitDstStageMask = waitDstStageMasksBegin;
        vkSubmitInfo.signalSemaphoreCount  = signalSemaphoreCount;
        vkSubmitInfo.pSignalSemaphores = signalSemaphoresBegin;
        vkSubmitInfo.commandBufferCount = cmd.index();
        vkSubmitInfo.pCommandBuffers = cmd.data();

        cmd.timelineValue() = timelineValue;
    }

    writePtr = reinterpret_cast<std::byte*>(submitInfoPtr);

    CHECK_VK(vkQueueSubmit(
        submitInfo.queue ? submitInfo.queue : myCommandContextDesc.deviceContext->getSelectedQueue(),
        myPendingCommands.size(),
        submitInfoBegin,
        submitInfo.signalFence));

    enqueueAllPendingToSubmitted(timelineValue);

    return timelineValue;
}

template <>
void CommandContext<GraphicsBackend::Vulkan>::sync(std::optional<uint64_t> timelineValue)
{
    uint64_t value = timelineValue.value_or(myLastSubmitTimelineValue.value_or(0));

    if (!hasReached(value))
    {
        VkSemaphoreWaitInfo waitInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
        waitInfo.flags = 0;
        waitInfo.semaphoreCount = 1;
        waitInfo.pSemaphores = &myCommandContextDesc.timelineSemaphore;
        waitInfo.pValues = &value;

        CHECK_VK(vkWaitSemaphores(myCommandContextDesc.deviceContext->getDevice(), &waitInfo, UINT64_MAX));
    }

    while (!mySyncCallbacks.empty() && hasReached(mySyncCallbacks.begin()->second))
    {
        mySyncCallbacks.begin()->first();
        mySyncCallbacks.pop_front();
    }
}

template <>
uint64_t CommandContext<GraphicsBackend::Vulkan>::execute(
    CommandContext<GraphicsBackend::Vulkan>& other,
    const RenderPassBeginInfo<GraphicsBackend::Vulkan>* beginInfo)
{
	{
		auto cmd = commands();

		vkCmdBeginRenderPass(cmd, beginInfo, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);

		for (const auto& secPendingCommands : other.myPendingCommands)
			vkCmdExecuteCommands(cmd, secPendingCommands.index(), secPendingCommands.data());

		vkCmdEndRenderPass(cmd);
	}

    auto timelineValue =
		other.myCommandContextDesc.timelineValue->fetch_add(1, std::memory_order_relaxed);

	other.enqueueAllPendingToSubmitted(timelineValue);

    return timelineValue;
}

template <>
CommandBufferArray<GraphicsBackend::Vulkan>::CommandBufferArray(CommandBufferArrayDesc<GraphicsBackend::Vulkan>&& desc)
: myArrayDesc(std::move(desc))
{
    VkCommandBufferAllocateInfo cmdInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cmdInfo.commandPool = myArrayDesc.commandPool;
    cmdInfo.level = static_cast<VkCommandBufferLevel>(myArrayDesc.commandBufferLevel);
    cmdInfo.commandBufferCount = kCommandBufferCount;
    CHECK_VK(vkAllocateCommandBuffers(
        myArrayDesc.deviceContext->getDevice(),
        &cmdInfo,
        myCommandBufferArray));
}

template <>
void CommandContext<GraphicsBackend::Vulkan>::enqueueOnePending()
{
    if (!myFreeCommands.empty())
        myPendingCommands.splice(myPendingCommands.end(), std::move(myFreeCommands), myFreeCommands.begin());
    else
        myPendingCommands.emplace_back(CommandBufferArray<GraphicsBackend::Vulkan>(
            CommandBufferArrayDesc<GraphicsBackend::Vulkan>{
                myCommandContextDesc.deviceContext,
                myCommandContextDesc.commandPool,
                myCommandContextDesc.commandBufferLevel}));
}

template <>
CommandContext<GraphicsBackend::Vulkan>::CommandContext(CommandContextDesc<GraphicsBackend::Vulkan>&& desc)
: myCommandContextDesc(std::move(desc))
{
}

template <>
CommandContext<GraphicsBackend::Vulkan>::~CommandContext()
{
}

template <>
CommandBufferArray<GraphicsBackend::Vulkan>::~CommandBufferArray()
{
    if (myCommandBufferArray[0])
        vkFreeCommandBuffers(
            myArrayDesc.deviceContext->getDevice(),
            myArrayDesc.commandPool,
            kCommandBufferCount,
            myCommandBufferArray);
}

template <>
void CommandBufferArray<GraphicsBackend::Vulkan>::begin(
    const CommandBufferBeginInfo<GraphicsBackend::Vulkan>* beginInfo) const
{
    VkCommandBufferBeginInfo defaultBeginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    defaultBeginInfo.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    assert(myIndex < kCommandBufferCount);

    CHECK_VK(vkResetCommandBuffer(myCommandBufferArray[myIndex], 0));
    CHECK_VK(vkBeginCommandBuffer(myCommandBufferArray[myIndex], beginInfo ? beginInfo : &defaultBeginInfo));
}

template <>
bool CommandBufferArray<GraphicsBackend::Vulkan>::end()
{
    assert(myIndex < kCommandBufferCount);

    CHECK_VK(vkEndCommandBuffer(myCommandBufferArray[myIndex]));

    return (++myIndex == kCommandBufferCount);
}
