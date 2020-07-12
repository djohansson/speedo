#include "command.h"
#include "vk-utils.h"

#include <core/slang-secure-crt.h>


namespace commandbufferarray_vulkan
{

static auto createArray(
    const std::shared_ptr<DeviceContext<GraphicsBackend::Vulkan>>& deviceContext,
    const CommandBufferArrayCreateDesc<GraphicsBackend::Vulkan>& desc)
{
    std::array<CommandBufferHandle<GraphicsBackend::Vulkan>, CommandBufferArray<GraphicsBackend::Vulkan>::kCommandBufferCount> outArray;

    VkCommandBufferAllocateInfo cmdInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cmdInfo.commandPool = desc.pool;
    cmdInfo.level = desc.level;
    cmdInfo.commandBufferCount = CommandBufferArray<GraphicsBackend::Vulkan>::kCommandBufferCount;
    VK_CHECK(vkAllocateCommandBuffers(
        deviceContext->getDevice(),
        &cmdInfo,
        outArray.data()));
    
    return outArray;
}

}

template <>
CommandBufferArray<GraphicsBackend::Vulkan>::CommandBufferArray(
    const std::shared_ptr<DeviceContext<GraphicsBackend::Vulkan>>& deviceContext,
    std::tuple<
        CommandBufferArrayCreateDesc<GraphicsBackend::Vulkan>,
        std::array<CommandBufferHandle<GraphicsBackend::Vulkan>,
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
CommandBufferArray<GraphicsBackend::Vulkan>::CommandBufferArray(
    const std::shared_ptr<DeviceContext<GraphicsBackend::Vulkan>>& deviceContext,
    CommandBufferArrayCreateDesc<GraphicsBackend::Vulkan>&& desc)
: CommandBufferArray(deviceContext, std::make_tuple(std::move(desc), commandbufferarray_vulkan::createArray(deviceContext, desc)))
{
}

template <>
CommandBufferArray<GraphicsBackend::Vulkan>::~CommandBufferArray()
{
    if (auto deviceContext = getDeviceContext())
        vkFreeCommandBuffers(
            deviceContext->getDevice(),
            myDesc.pool,
            kCommandBufferCount,
            myArray.data());
}

template <>
void CommandBufferArray<GraphicsBackend::Vulkan>::resetAll()
{
    assert(!recordingFlags());
    assert(head() < kCommandBufferCount);
    
    for (uint32_t i = 0; i < myBits.head; i++)
        VK_CHECK(vkResetCommandBuffer(myArray[i], VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT));

    myBits = {0, 0};
}

template <>
void CommandContext<GraphicsBackend::Vulkan>::enqueueOnePending(CommandBufferLevel<GraphicsBackend::Vulkan> level)
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
            
        myPendingCommands[level].emplace_back(std::make_pair(CommandBufferArray<GraphicsBackend::Vulkan>(
            myDevice,
            CommandBufferArrayCreateDesc<GraphicsBackend::Vulkan>{
                {stringBuffer},
                myDesc.pool,
                level}), std::make_pair(0, std::reference_wrapper(*this))));
    }
}

template <>
CommandContextBeginInfo<GraphicsBackend::Vulkan>::CommandContextBeginInfo()
: CommandBufferBeginInfo<GraphicsBackend::Vulkan>{
    VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    nullptr,
    VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    nullptr}
{
}

template <>
CommandBufferHandle<GraphicsBackend::Vulkan> CommandContext<GraphicsBackend::Vulkan>::internalBeginScope(
    CommandContextBeginInfo<GraphicsBackend::Vulkan>&& beginInfo)
{
    if (myPendingCommands[beginInfo.level].empty() || myPendingCommands[beginInfo.level].back().first.full())
        enqueueOnePending(beginInfo.level);

    myLastCommands.emplace(
        CommandBufferAccessScope<GraphicsBackend::Vulkan>(myPendingCommands[beginInfo.level].back().first, std::move(beginInfo)));

    return (*myLastCommands);
}
    
template <>
CommandBufferHandle<GraphicsBackend::Vulkan> CommandContext<GraphicsBackend::Vulkan>::internalCommands() const
{
    return (*myLastCommands);
}

template <>
void CommandContext<GraphicsBackend::Vulkan>::enqueueExecuted(CommandBufferList&& commands, uint64_t timelineValue)
{
    for (auto& cmd : commands)
        cmd.second.first = timelineValue;

    myExecutedCommands.splice(myExecutedCommands.end(), std::move(commands));

    myDevice->addTimelineCompletionCallback(timelineValue, [this](uint64_t timelineValue)
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
void CommandContext<GraphicsBackend::Vulkan>::addSubmitFinishedCallback(std::function<void(uint64_t)>&& callback)
{
    mySubmitFinishedCallbacks.emplace_back(std::move(callback));
}

template <>
void CommandContext<GraphicsBackend::Vulkan>::enqueueSubmitted(CommandBufferList&& commands, uint64_t timelineValue)
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

    myDevice->addTimelineCompletionCallbacks(timelineValue, mySubmitFinishedCallbacks);
    mySubmitFinishedCallbacks.clear();
}

template <>
uint64_t CommandContext<GraphicsBackend::Vulkan>::submit(
    const CommandSubmitInfo<GraphicsBackend::Vulkan>& submitInfo)
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
        sizeof(SemaphoreHandle<GraphicsBackend::Vulkan>) * signalSemaphoreCount +
        sizeof(uint64_t) * waitSemaphoreCount +
        sizeof(SemaphoreHandle<GraphicsBackend::Vulkan>) * waitSemaphoreCount +
        sizeof(Flags<GraphicsBackend::Vulkan>) * waitSemaphoreCount +
        sizeof(CommandBufferHandle<GraphicsBackend::Vulkan>) * pendingCommands.size() * CommandBufferArray<GraphicsBackend::Vulkan>::kCommandBufferCount);

    auto writePtr = myScratchMemory.data();

    auto waitSemaphoresBegin = reinterpret_cast<SemaphoreHandle<GraphicsBackend::Vulkan>*>(writePtr);
    {
        std::copy_n(submitInfo.waitSemaphores, submitInfo.waitSemaphoreCount, waitSemaphoresBegin);
        writePtr = reinterpret_cast<std::byte*>(waitSemaphoresBegin + submitInfo.waitSemaphoreCount);
    }

    auto waitDstStageMasksBegin = reinterpret_cast<Flags<GraphicsBackend::Vulkan>*>(writePtr);
    {
        std::copy_n(submitInfo.waitDstStageMasks, submitInfo.waitSemaphoreCount, waitDstStageMasksBegin);
        writePtr = reinterpret_cast<std::byte*>(waitDstStageMasksBegin + submitInfo.waitSemaphoreCount);
    }

    auto waitSemaphoreValuesBegin = reinterpret_cast<uint64_t*>(writePtr);
    {
        std::copy_n(submitInfo.waitSemaphoreValues, submitInfo.waitSemaphoreCount, waitSemaphoreValuesBegin);
        writePtr = reinterpret_cast<std::byte*>(waitSemaphoreValuesBegin + submitInfo.waitSemaphoreCount);
    }
    
    auto signalSemaphoresBegin = reinterpret_cast<SemaphoreHandle<GraphicsBackend::Vulkan>*>(writePtr);
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
    auto commandBufferHandlesBegin = reinterpret_cast<CommandBufferHandle<GraphicsBackend::Vulkan>*>(writePtr);
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
uint64_t CommandContext<GraphicsBackend::Vulkan>::execute(CommandContext<GraphicsBackend::Vulkan>& callee)
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
uint8_t CommandBufferArray<GraphicsBackend::Vulkan>::begin(
    const CommandBufferBeginInfo<GraphicsBackend::Vulkan>& beginInfo)
{
    assert(!recording(myBits.head));
    assert(!full());
    
    VK_CHECK(vkBeginCommandBuffer(myArray[myBits.head], &beginInfo));

    myBits.recordingFlags |= (1 << myBits.head);

    return static_cast<uint8_t>(myBits.head++);
}

template <>
void CommandBufferArray<GraphicsBackend::Vulkan>::end(uint8_t index)
{
    assert(recording(index));

    myBits.recordingFlags &= ~(1 << index);

    VK_CHECK(vkEndCommandBuffer(myArray[index]));
}

template <>
CommandContext<GraphicsBackend::Vulkan>::CommandContext(
    const std::shared_ptr<DeviceContext<GraphicsBackend::Vulkan>>& deviceContext,
    CommandContextCreateDesc<GraphicsBackend::Vulkan>&& desc)
: myDevice(deviceContext)
, myDesc(std::move(desc))
, myPendingCommands(2)
, myFreeCommands(2)
{
    ZoneScopedN("CommandContext()");
}

template <>
CommandContext<GraphicsBackend::Vulkan>::~CommandContext()
{
    ZoneScopedN("~CommandContext()");

    if (!mySubmittedCommands.empty())
    {
        myDevice->wait(mySubmittedCommands.back().second.first);
        myDevice->processTimelineCallbacks(mySubmittedCommands.back().second.first);
    }
}
