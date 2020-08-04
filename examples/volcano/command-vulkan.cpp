#include "command.h"
#include "vk-utils.h"

#include <core/slang-secure-crt.h>

namespace commandbufferarray
{

static auto createArray(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    const CommandBufferArrayCreateDesc<Vk>& desc)
{
    std::array<CommandBufferHandle<Vk>, CommandBufferArray<Vk>::kCommandBufferCount> outArray;

    VkCommandBufferAllocateInfo cmdInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cmdInfo.commandPool = desc.pool;
    cmdInfo.level = desc.level;
    cmdInfo.commandBufferCount = CommandBufferArray<Vk>::kCommandBufferCount;
    VK_CHECK(vkAllocateCommandBuffers(
        deviceContext->getDevice(),
        &cmdInfo,
        outArray.data()));
    
    return outArray;
}

}

template <>
CommandBufferArray<Vk>::CommandBufferArray(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    std::tuple<
        CommandBufferArrayCreateDesc<Vk>,
        std::array<CommandBufferHandle<Vk>,
        kCommandBufferCount>>&& descAndData)
: DeviceResource(
    deviceContext,
    std::get<0>(descAndData),
    kCommandBufferCount,
    VK_OBJECT_TYPE_COMMAND_BUFFER, 
    reinterpret_cast<uint64_t*>(std::get<1>(descAndData).data()))
, myDesc(std::move(std::get<0>(descAndData)))
, myArray(std::move(std::get<1>(descAndData)))
{
}

template <>
CommandBufferArray<Vk>::CommandBufferArray(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    CommandBufferArrayCreateDesc<Vk>&& desc)
: CommandBufferArray(deviceContext, std::make_tuple(std::move(desc), commandbufferarray::createArray(deviceContext, desc)))
{
}

template <>
CommandBufferArray<Vk>::~CommandBufferArray()
{
    if (auto deviceContext = getDeviceContext())
        vkFreeCommandBuffers(
            deviceContext->getDevice(),
            myDesc.pool,
            kCommandBufferCount,
            myArray.data());
}

template <>
void CommandBufferArray<Vk>::resetAll()
{
    assert(!recordingFlags());
    assert(head() < kCommandBufferCount);
    
    for (uint32_t i = 0; i < myBits.head; i++)
        VK_CHECK(vkResetCommandBuffer(myArray[i], VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT));

    myBits = {0, 0};
}

template <>
void CommandContext<Vk>::enqueueOnePending(CommandBufferLevel<Vk> level)
{
    if (!myFreeCommands[level].empty())
    {
        myPendingCommands[level].splice(myPendingCommands[level].end(), std::move(myFreeCommands[level]), myFreeCommands[level].begin());
    }
    else
    {
        char stringBuffer[32];

        sprintf_s(
            stringBuffer,
            sizeof(stringBuffer),
            "%s%s",
            level == VK_COMMAND_BUFFER_LEVEL_PRIMARY ? "Primary" : "Secondary",
            "CommandBufferArray");
            
        myPendingCommands[level].emplace_back(std::make_pair(CommandBufferArray<Vk>(
            myDevice,
            CommandBufferArrayCreateDesc<Vk>{
                {stringBuffer},
                myDesc.pool,
                level}), std::make_pair(0, std::reference_wrapper(*this))));
    }
}

template <>
CommandContextBeginInfo<Vk>::CommandContextBeginInfo()
: CommandBufferBeginInfo<Vk>{
    VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    nullptr,
    VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    nullptr}
{
}

template <>
bool CommandContextBeginInfo<Vk>::operator==(const CommandContextBeginInfo& other) const
{
    bool result = other.flags == flags && other.level == level;
    if (result && level == VK_COMMAND_BUFFER_LEVEL_SECONDARY)
    {
        assert(pInheritanceInfo);
        result &= 
            other.pInheritanceInfo->renderPass == pInheritanceInfo->renderPass &&
            other.pInheritanceInfo->subpass == pInheritanceInfo->subpass &&
            other.pInheritanceInfo->framebuffer == pInheritanceInfo->framebuffer &&
            other.pInheritanceInfo->occlusionQueryEnable == pInheritanceInfo->occlusionQueryEnable &&
            other.pInheritanceInfo->queryFlags == pInheritanceInfo->queryFlags && 
            other.pInheritanceInfo->pipelineStatistics == pInheritanceInfo->pipelineStatistics;
    }

    return result;
}

template <>
CommandBufferHandle<Vk> CommandContext<Vk>::internalBeginScope(
    CommandContextBeginInfo<Vk>&& beginInfo)
{
    if (myPendingCommands[beginInfo.level].empty() || myPendingCommands[beginInfo.level].back().first.full())
        enqueueOnePending(beginInfo.level);

    myRecordingCommands.emplace(
        CommandBufferAccessScope(
            myPendingCommands[beginInfo.level].back().first,
            std::move(beginInfo)));

    return (*myRecordingCommands);
}
    
template <>
CommandBufferHandle<Vk> CommandContext<Vk>::internalCommands() const
{
    return (*myRecordingCommands);
}

template <>
void CommandContext<Vk>::enqueueExecuted(CommandBufferList&& commands, uint64_t timelineValue)
{
    for (auto& cmd : commands)
        cmd.second.first = timelineValue;

    myExecutedCommands.splice(myExecutedCommands.end(), std::move(commands));

    myDevice->addTimelineCallback(timelineValue, [this](uint64_t timelineValue)
    {
        ZoneScopedN("cmdReset");

        auto onResetCommands = [](CommandBufferList& from, uint64_t timelineValue)
        {
            auto fromBeginIt = from.begin();
            auto fromIt = fromBeginIt;

            while (fromIt != from.end() && fromIt->second.first < timelineValue)
            {
                fromIt->first.resetAll();
                auto& toContext = fromIt->second.second.get();
                auto& to = toContext.myFreeCommands[fromIt->first.getDesc().level];                
                auto toIt = fromIt++;
                to.splice(to.end(), std::move(from), toIt);
            }
        };

        onResetCommands(myExecutedCommands, timelineValue);
    });
}

template <>
void CommandContext<Vk>::addSubmitFinishedCallback(std::function<void(uint64_t)>&& callback)
{
    mySubmitFinishedCallbacks.emplace_back(std::move(callback));
}

template <>
void CommandContext<Vk>::enqueueSubmitted(CommandBufferList&& commands, uint64_t timelineValue)
{
    for (auto& cmd : commands)
        cmd.second.first = timelineValue;

    mySubmittedCommands.splice(mySubmittedCommands.end(), std::move(commands));

    addSubmitFinishedCallback([this](uint64_t timelineValue)
    {
        ZoneScopedN("cmdReset");

        auto onResetCommands = [](CommandBufferList& from, CommandBufferList& to, uint64_t timelineValue)
        {
            auto fromBeginIt = from.begin();
            auto fromIt = fromBeginIt;

            while (fromIt != from.end() && fromIt->second.first < timelineValue)
                (fromIt++)->first.resetAll();

            to.splice(to.end(), std::move(from), fromBeginIt, fromIt);
        };

        onResetCommands(mySubmittedCommands, myFreeCommands[VK_COMMAND_BUFFER_LEVEL_PRIMARY], timelineValue);
    });

    myDevice->addTimelineCallbacks(timelineValue, mySubmitFinishedCallbacks);
    mySubmitFinishedCallbacks.clear();
}

template <>
uint64_t CommandContext<Vk>::submit(
    const CommandSubmitInfo<Vk>& submitInfo)
{
    ZoneScopedN("submit");

    std::unique_lock writeLock(myCommandsMutex);

    endCommands();

    auto& pendingCommands = myPendingCommands[VK_COMMAND_BUFFER_LEVEL_PRIMARY];

    if (pendingCommands.empty())
        return 0;

    const auto waitSemaphoreCount = submitInfo.waitSemaphoreCount;
    const auto signalSemaphoreCount = submitInfo.signalSemaphoreCount;

    myScratchMemory.resize(
        sizeof(uint64_t) * signalSemaphoreCount + 
        sizeof(SemaphoreHandle<Vk>) * signalSemaphoreCount +
        sizeof(uint64_t) * waitSemaphoreCount +
        sizeof(SemaphoreHandle<Vk>) * waitSemaphoreCount +
        sizeof(Flags<Vk>) * waitSemaphoreCount +
        sizeof(CommandBufferHandle<Vk>) * pendingCommands.size() * CommandBufferArray<Vk>::kCommandBufferCount);

    auto writePtr = myScratchMemory.data();

    auto waitSemaphoresBegin = reinterpret_cast<SemaphoreHandle<Vk>*>(writePtr);
    {
        std::copy_n(submitInfo.waitSemaphores, submitInfo.waitSemaphoreCount, waitSemaphoresBegin);
        writePtr = reinterpret_cast<std::byte*>(waitSemaphoresBegin + submitInfo.waitSemaphoreCount);
    }

    auto waitDstStageMasksBegin = reinterpret_cast<Flags<Vk>*>(writePtr);
    {
        std::copy_n(submitInfo.waitDstStageMasks, submitInfo.waitSemaphoreCount, waitDstStageMasksBegin);
        writePtr = reinterpret_cast<std::byte*>(waitDstStageMasksBegin + submitInfo.waitSemaphoreCount);
    }

    auto waitSemaphoreValuesBegin = reinterpret_cast<uint64_t*>(writePtr);
    {
        std::copy_n(submitInfo.waitSemaphoreValues, submitInfo.waitSemaphoreCount, waitSemaphoreValuesBegin);
        writePtr = reinterpret_cast<std::byte*>(waitSemaphoreValuesBegin + submitInfo.waitSemaphoreCount);
    }
    
    auto signalSemaphoresBegin = reinterpret_cast<SemaphoreHandle<Vk>*>(writePtr);
    {
        std::copy_n(submitInfo.signalSemaphores, submitInfo.signalSemaphoreCount, signalSemaphoresBegin);
        writePtr = reinterpret_cast<std::byte*>(signalSemaphoresBegin + submitInfo.signalSemaphoreCount);
    }

    auto signalSemaphoreValuesBegin = reinterpret_cast<uint64_t*>(writePtr);
    {
        std::copy_n(submitInfo.signalSemaphoreValues, submitInfo.signalSemaphoreCount, signalSemaphoreValuesBegin);
        writePtr = reinterpret_cast<std::byte*>(signalSemaphoreValuesBegin + submitInfo.signalSemaphoreCount);
    }

    uint32_t commandBufferHandlesCount = 0;
    auto commandBufferHandlesBegin = reinterpret_cast<CommandBufferHandle<Vk>*>(writePtr);
    {
        auto commandBufferHandlesPtr = commandBufferHandlesBegin;
        for (auto& cmdSegment : pendingCommands)
        {
            assert(!cmdSegment.first.recordingFlags());
            auto cmdCount = cmdSegment.first.head();
            commandBufferHandlesCount += cmdCount;
            std::copy_n(cmdSegment.first.data(), cmdCount, commandBufferHandlesPtr);
            commandBufferHandlesPtr += cmdCount;
        }
    }

    VkTimelineSemaphoreSubmitInfo timelineInfo;
    timelineInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    timelineInfo.pNext = nullptr;
    timelineInfo.waitSemaphoreValueCount = waitSemaphoreCount;
    timelineInfo.pWaitSemaphoreValues = waitSemaphoreValuesBegin;
    timelineInfo.signalSemaphoreValueCount = signalSemaphoreCount;
    timelineInfo.pSignalSemaphoreValues = signalSemaphoreValuesBegin;
    
    VkSubmitInfo vkSubmitInfo;
    vkSubmitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    vkSubmitInfo.pNext = &timelineInfo;
    vkSubmitInfo.waitSemaphoreCount = waitSemaphoreCount;
    vkSubmitInfo.pWaitSemaphores = waitSemaphoresBegin;
    vkSubmitInfo.pWaitDstStageMask = waitDstStageMasksBegin;
    vkSubmitInfo.signalSemaphoreCount  = signalSemaphoreCount;
    vkSubmitInfo.pSignalSemaphores = signalSemaphoresBegin;
    vkSubmitInfo.commandBufferCount = commandBufferHandlesCount;
    vkSubmitInfo.pCommandBuffers = commandBufferHandlesBegin;

    VK_CHECK(vkQueueSubmit(submitInfo.queue, 1, &vkSubmitInfo, submitInfo.signalFence));

    const auto [minSignalValue, maxSignalValue] = std::minmax_element(
        &submitInfo.signalSemaphoreValues[0],
        &submitInfo.signalSemaphoreValues[submitInfo.signalSemaphoreCount - 1]);

    enqueueSubmitted(std::move(pendingCommands), *maxSignalValue);
    
    return *maxSignalValue;
}

template <>
uint64_t CommandContext<Vk>::execute(CommandContext<Vk>& callee)
{
    ZoneScopedN("execute");

    if (&callee != this)
        std::lock(myCommandsMutex, callee.myCommandsMutex);
    else
        myCommandsMutex.lock();

    {
		auto cmd = internalCommands();

		for (const auto& secPendingCommands : callee.myPendingCommands[VK_COMMAND_BUFFER_LEVEL_SECONDARY])
			vkCmdExecuteCommands(cmd, secPendingCommands.first.head(), secPendingCommands.first.data());
	}

    auto timelineValue = myDevice->timelineValue().load(std::memory_order_relaxed);

	enqueueExecuted(std::move(callee.myPendingCommands[VK_COMMAND_BUFFER_LEVEL_SECONDARY]), timelineValue);
    
    myCommandsMutex.unlock();

    if (&callee != this)
        callee.myCommandsMutex.unlock();

    return timelineValue;
}

template <>
uint8_t CommandBufferArray<Vk>::begin(
    const CommandBufferBeginInfo<Vk>& beginInfo)
{
    assert(!recording(myBits.head));
    assert(!full());
    
    VK_CHECK(vkBeginCommandBuffer(myArray[myBits.head], &beginInfo));

    myBits.recordingFlags |= (1 << myBits.head);

    return static_cast<uint8_t>(myBits.head++);
}

template <>
void CommandBufferArray<Vk>::end(uint8_t index)
{
    assert(recording(index));

    myBits.recordingFlags &= ~(1 << index);

    VK_CHECK(vkEndCommandBuffer(myArray[index]));
}

template <>
CommandContext<Vk>::CommandContext(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    CommandContextCreateDesc<Vk>&& desc)
: myDevice(deviceContext)
, myDesc(std::move(desc))
, myPendingCommands(2)
, myFreeCommands(2)
{
    ZoneScopedN("CommandContext()");
}

template <>
CommandContext<Vk>::~CommandContext()
{
    ZoneScopedN("~CommandContext()");

    if (!mySubmittedCommands.empty())
    {
        myDevice->wait(mySubmittedCommands.back().second.first);
        myDevice->processTimelineCallbacks(mySubmittedCommands.back().second.first);
    }
}
