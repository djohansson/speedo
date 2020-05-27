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
    cmdInfo.level = static_cast<VkCommandBufferLevel>(desc.level);
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
            myDeviceContext,
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
CommandBufferAccessScope<GraphicsBackend::Vulkan>
CommandContext<GraphicsBackend::Vulkan>::internalBeginScope(const CommandContextBeginInfo<GraphicsBackend::Vulkan>& beginInfo)
{
    if (myPendingCommands[beginInfo.level].empty() || myPendingCommands[beginInfo.level].back().first.full())
        enqueueOnePending(beginInfo.level);

    auto scope = CommandBufferAccessScope<GraphicsBackend::Vulkan>(myPendingCommands[beginInfo.level].back().first, beginInfo);

    myLastCommands = scope;

    return scope;
}
    
template <>
CommandBufferHandle<GraphicsBackend::Vulkan> CommandContext<GraphicsBackend::Vulkan>::internalCommands() const
{
    return myLastCommands;
}

template <>
void CommandContext<GraphicsBackend::Vulkan>::enqueueExecuted(CommandBufferList&& commands, uint64_t timelineValue)
{
    for (auto& cmd : commands)
        cmd.second.first = timelineValue;

    myExecutedCommands.splice(myExecutedCommands.end(), std::move(commands));

    myDeviceContext->addGarbageCollectCallback(timelineValue, [this](uint64_t timelineValue)
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
void CommandContext<GraphicsBackend::Vulkan>::enqueueSubmitted(CommandBufferList&& commands, uint64_t timelineValue)
{
    for (auto& cmd : commands)
        cmd.second.first = timelineValue;

    mySubmittedCommands.splice(mySubmittedCommands.end(), std::move(commands));

    myDeviceContext->addGarbageCollectCallback(timelineValue, [this](uint64_t timelineValue)
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
}

template <>
uint64_t CommandContext<GraphicsBackend::Vulkan>::submit(
    const CommandSubmitInfo<GraphicsBackend::Vulkan>& submitInfo)
{
    std::unique_lock writeLock(myCommandsMutex);

    ZoneScopedN("submit");

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
        sizeof(VkTimelineSemaphoreSubmitInfo) * pendingCommands.size() + 
        sizeof(VkSubmitInfo) * pendingCommands.size());

    auto writePtr = myScratchMemory.data();

    auto waitSemaphoresBegin = reinterpret_cast<SemaphoreHandle<GraphicsBackend::Vulkan>*>(writePtr);
    {
        auto waitSemaphoresPtr = waitSemaphoresBegin;
        for (uint32_t i = 0; i < submitInfo.waitSemaphoreCount; i++)
            *waitSemaphoresPtr++ = submitInfo.waitSemaphores[i];

        writePtr = reinterpret_cast<std::byte*>(waitSemaphoresPtr);
    }

    auto waitDstStageMasksBegin = reinterpret_cast<Flags<GraphicsBackend::Vulkan>*>(writePtr);
    {
        auto waitDstStageMasksPtr = waitDstStageMasksBegin;
        for (uint32_t i = 0; i < submitInfo.waitSemaphoreCount; i++)
            *waitDstStageMasksPtr++ = submitInfo.waitDstStageMasks[i];

        writePtr = reinterpret_cast<std::byte*>(waitDstStageMasksPtr);
    }

    auto waitSemaphoreValuesBegin = reinterpret_cast<uint64_t*>(writePtr);
    {
        auto waitSemaphoreValuesPtr = waitSemaphoreValuesBegin;
        for (uint32_t i = 0; i < submitInfo.waitSemaphoreCount; i++)
            *waitSemaphoreValuesPtr++ = submitInfo.waitSemaphoreValues[i];

        writePtr = reinterpret_cast<std::byte*>(waitSemaphoreValuesPtr);
    }
    
    auto signalSemaphoresBegin = reinterpret_cast<SemaphoreHandle<GraphicsBackend::Vulkan>*>(writePtr);
    {
        auto signalSemaphoresPtr = signalSemaphoresBegin;
        for (uint32_t i = 0; i < submitInfo.signalSemaphoreCount; i++)
            *signalSemaphoresPtr++ = submitInfo.signalSemaphores[i];

        writePtr = reinterpret_cast<std::byte*>(signalSemaphoresPtr);
    }

    uint64_t maxSignalValue = 0;
    auto signalSemaphoreValuesBegin = reinterpret_cast<uint64_t*>(writePtr);
    {
        auto signalSemaphoreValuesPtr = signalSemaphoreValuesBegin;
        for (uint32_t i = 0; i < submitInfo.signalSemaphoreCount; i++)
        {
            uint64_t signalValue = submitInfo.signalSemaphoreValues[i];
            maxSignalValue = std::max(maxSignalValue, signalValue);
            *signalSemaphoreValuesPtr++ = signalValue;
        }

        writePtr = reinterpret_cast<std::byte*>(signalSemaphoreValuesPtr);
    }

    auto timelineSemaphoreSubmitInfoBegin = reinterpret_cast<VkTimelineSemaphoreSubmitInfo*>(writePtr);
    auto timelineSemaphoreSubmitInfoPtr = timelineSemaphoreSubmitInfoBegin;
    auto submitInfoBegin = reinterpret_cast<VkSubmitInfo*>(writePtr + sizeof(VkTimelineSemaphoreSubmitInfo) * pendingCommands.size());
    auto submitInfoPtr = submitInfoBegin;

    for (auto& cmd : pendingCommands)
    {
        assert(!cmd.first.recordingFlags());

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
        vkSubmitInfo.commandBufferCount = cmd.first.head();
        vkSubmitInfo.pCommandBuffers = cmd.first.data();
    }

    VK_CHECK(vkQueueSubmit(submitInfo.queue, pendingCommands.size(), submitInfoBegin, submitInfo.signalFence));
    
    return maxSignalValue;
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

    auto timelineValue = myDeviceContext->timelineValue()->load(std::memory_order_relaxed);

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
: myDeviceContext(deviceContext)
, myDesc(std::move(desc))
, myPendingCommands(VK_COMMAND_BUFFER_LEVEL_RANGE_SIZE)
, myFreeCommands(VK_COMMAND_BUFFER_LEVEL_RANGE_SIZE)
{
    ZoneScopedN("CommandContext()");
}

template <>
CommandContext<GraphicsBackend::Vulkan>::~CommandContext()
{
    ZoneScopedN("~CommandContext()");

    if (!mySubmittedCommands.empty())
    {
        myDeviceContext->wait(mySubmittedCommands.back().second.first);
        myDeviceContext->collectGarbage(mySubmittedCommands.back().second.first);
    }
}
