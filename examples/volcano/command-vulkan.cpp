#include "command.h"
#include "vk-utils.h"


template <>
auto CommandBufferArray<GraphicsBackend::Vulkan>::createArray(
    const std::shared_ptr<DeviceContext<GraphicsBackend::Vulkan>>& deviceContext,
    const CommandBufferArrayCreateDesc<GraphicsBackend::Vulkan>& desc)
{
    std::array<CommandBufferHandle<GraphicsBackend::Vulkan>, kCommandBufferCount> outArray;

    VkCommandBufferAllocateInfo cmdInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cmdInfo.commandPool = desc.commandPool;
    cmdInfo.level = static_cast<VkCommandBufferLevel>(desc.commandBufferLevel);
    cmdInfo.commandBufferCount = kCommandBufferCount;
    CHECK_VK(vkAllocateCommandBuffers(
        deviceContext->getDevice(),
        &cmdInfo,
        outArray.data()));
    
    return outArray;
}

template <>
CommandBufferArray<GraphicsBackend::Vulkan>::CommandBufferArray(
    const std::shared_ptr<DeviceContext<GraphicsBackend::Vulkan>>& deviceContext,
    std::tuple<
        CommandBufferArrayCreateDesc<GraphicsBackend::Vulkan>,
        std::array<CommandBufferHandle<GraphicsBackend::Vulkan>,
        kCommandBufferCount>>&& descAndData)
: Resource(
    deviceContext,
    std::get<0>(descAndData),
    VK_OBJECT_TYPE_COMMAND_BUFFER, 
    kCommandBufferCount,
    reinterpret_cast<uint64_t*>(std::get<1>(descAndData).data()))
, myDesc(std::move(std::get<0>(descAndData)))
, myCommandBufferArray(std::move(std::get<1>(descAndData)))
{
}

template <>
CommandBufferArray<GraphicsBackend::Vulkan>::CommandBufferArray(
    const std::shared_ptr<DeviceContext<GraphicsBackend::Vulkan>>& deviceContext,
    CommandBufferArrayCreateDesc<GraphicsBackend::Vulkan>&& desc)
: CommandBufferArray(deviceContext, std::make_tuple(std::move(desc), createArray(deviceContext, desc)))
{
}

template <>
CommandBufferArray<GraphicsBackend::Vulkan>::~CommandBufferArray()
{
    if (getDeviceContext())
        vkFreeCommandBuffers(
            getDeviceContext()->getDevice(),
            myDesc.commandPool,
            kCommandBufferCount,
            myCommandBufferArray.data());
}

template <>
void CommandBufferArray<GraphicsBackend::Vulkan>::reset()
{
    assert(!myBits.myRecording);
    assert(myBits.myHead < kCommandBufferCount);
    
    for (uint32_t i = 0; i <= myBits.myHead; i++)
        CHECK_VK(vkResetCommandBuffer(myCommandBufferArray[i], 0));

    // if (myConfig.useCommandPoolReset.value())
    // {
    //     ZoneScopedN("poolReset");

    //     CHECK_VK(vkResetCommandPool(
    //         myDevice,
    //         myDesc.commandPool,
    //         VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT));
    // }

    myBits.myHead = 0;
}


template <>
void CommandContext<GraphicsBackend::Vulkan>::enqueueOnePending()
{
    if (!myFreeCommands.empty())
        myPendingCommands.splice(myPendingCommands.end(), std::move(myFreeCommands), myFreeCommands.begin());
    else
        myPendingCommands.emplace_back(CommandBufferArray<GraphicsBackend::Vulkan>(
            myDeviceContext,
            CommandBufferArrayCreateDesc<GraphicsBackend::Vulkan>{
                {"CommandBufferArray"},
                myDesc.commandPool,
                myDesc.commandBufferLevel}));
}

template <>
void CommandContext<GraphicsBackend::Vulkan>::enqueueAllPendingToSubmitted(uint64_t timelineValue)
{
    mySubmittedCommands.splice(mySubmittedCommands.end(), std::move(myPendingCommands));

    myDeviceContext->addCommandBufferGarbageCollectCallback([this](uint64_t timelineValue)
    {
        ZoneScopedN("freeCmd");

        auto freeBeginIt = mySubmittedCommands.begin();
        auto freeEndIt = freeBeginIt;

        if (!myDeviceContext->getDesc().useCommandPoolReset.value())
        {
            ZoneScopedN("reset");

            while (freeEndIt != mySubmittedCommands.end())
                (freeEndIt++)->reset();
        }

        assert(mySubmittedCommands.size() < 100);
        
        myFreeCommands.splice(myFreeCommands.end(), std::move(mySubmittedCommands), freeBeginIt, freeEndIt);

    }, timelineValue);
}

template <>
uint64_t CommandContext<GraphicsBackend::Vulkan>::submit(
    const CommandSubmitInfo<GraphicsBackend::Vulkan>& submitInfo)
{
    ZoneScopedN("submit");

    assert(!myPendingCommands.empty());

    const auto waitSemaphoreCount = submitInfo.waitSemaphoreCount + 1;
    const auto signalSemaphoreCount = submitInfo.signalSemaphoreCount + 1;

    myScratchMemory.resize(
        sizeof(uint64_t) * signalSemaphoreCount + 
        sizeof(SemaphoreHandle<GraphicsBackend::Vulkan>) * signalSemaphoreCount +
        sizeof(uint64_t) * waitSemaphoreCount +
        sizeof(SemaphoreHandle<GraphicsBackend::Vulkan>) * waitSemaphoreCount +
        sizeof(Flags<GraphicsBackend::Vulkan>) * waitSemaphoreCount +
        sizeof(VkTimelineSemaphoreSubmitInfo) * myPendingCommands.size() + 
        sizeof(VkSubmitInfo) * myPendingCommands.size());

    auto writePtr = myScratchMemory.data();

    auto waitSemaphoresBegin = reinterpret_cast<SemaphoreHandle<GraphicsBackend::Vulkan>*>(writePtr);
    {
        auto waitSemaphoresPtr = waitSemaphoresBegin;
        for (uint32_t i = 0; i < submitInfo.waitSemaphoreCount; i++)
            *waitSemaphoresPtr++ = submitInfo.waitSemaphores[i];
        *waitSemaphoresPtr++ = myDeviceContext->getTimelineSemaphore();

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
        auto lastSubmitTimelineValue = myLastSubmitTimelineValue.value_or(0);
        for (uint32_t i = 0; i < submitInfo.waitSemaphoreCount; i++)
            *waitSemaphoreValuesPtr++ = submitInfo.waitSemaphoreValues ? submitInfo.waitSemaphoreValues[i] : lastSubmitTimelineValue;
        *waitSemaphoreValuesPtr++ = lastSubmitTimelineValue;

        writePtr = reinterpret_cast<std::byte*>(waitSemaphoreValuesPtr);
    }

    auto timelineValue = myDeviceContext->timelineValue()->fetch_add(signalSemaphoreCount, std::memory_order_relaxed);
    
    auto signalSemaphoresBegin = reinterpret_cast<SemaphoreHandle<GraphicsBackend::Vulkan>*>(writePtr);
    {
        auto signalSemaphoresPtr = signalSemaphoresBegin;
        for (uint32_t i = 0; i < submitInfo.signalSemaphoreCount; i++)
            *signalSemaphoresPtr++ = submitInfo.signalSemaphores[i];
        *signalSemaphoresPtr++ = myDeviceContext->getTimelineSemaphore();

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
        assert(!cmd.recording());

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
        vkSubmitInfo.commandBufferCount = cmd.size();
        vkSubmitInfo.pCommandBuffers = cmd.data();
    }

    writePtr = reinterpret_cast<std::byte*>(submitInfoPtr);

    CHECK_VK(vkQueueSubmit(submitInfo.queue, myPendingCommands.size(), submitInfoBegin, submitInfo.signalFence));

    enqueueAllPendingToSubmitted(timelineValue);

    myLastSubmitTimelineValue = timelineValue;

    return timelineValue;
}

template <>
uint64_t CommandContext<GraphicsBackend::Vulkan>::execute(CommandContext<GraphicsBackend::Vulkan>& other)
{
    ZoneScopedN("execute");

    {
		auto cmd = commands();

		for (const auto& secPendingCommands : other.myPendingCommands)
			vkCmdExecuteCommands(cmd, secPendingCommands.size(), secPendingCommands.data());
	}

    auto timelineValue = myDeviceContext->timelineValue()->fetch_add(1, std::memory_order_relaxed);

	other.enqueueAllPendingToSubmitted(timelineValue);

    return timelineValue;
}

template <>
void CommandBufferArray<GraphicsBackend::Vulkan>::begin(
    const CommandBufferBeginInfo<GraphicsBackend::Vulkan>* beginInfo)
{
    assert(!myBits.myRecording);
    assert(myBits.myHead < kCommandBufferCount);

    static auto defaultBeginInfo = VkCommandBufferBeginInfo{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        nullptr,
        VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        nullptr
    };
    
    CHECK_VK(vkBeginCommandBuffer(myCommandBufferArray[myBits.myHead], beginInfo ? beginInfo : &defaultBeginInfo));

    myBits.myRecording = true;
}

template <>
bool CommandBufferArray<GraphicsBackend::Vulkan>::end()
{
    assert(myBits.myRecording);
    assert(myBits.myHead < kCommandBufferCount);

    myBits.myRecording = false;

    CHECK_VK(vkEndCommandBuffer(myCommandBufferArray[myBits.myHead]));

    return (++myBits.myHead == kCommandBufferCount);
}

template <>
CommandContext<GraphicsBackend::Vulkan>::CommandContext(
    const std::shared_ptr<DeviceContext<GraphicsBackend::Vulkan>>& deviceContext,
    CommandContextCreateDesc<GraphicsBackend::Vulkan>&& desc)
: myDeviceContext(deviceContext)
, myDesc(std::move(desc))
{
    ZoneScopedN("CommandContext()");

    myUserData = command_vulkan::UserData();

// disabled as shared_from_this() throws exception. called from the outside for now
// {
//     if (myDesc.commandBufferLevel == 0)
//         std::any_cast<command_vulkan::UserData>(&myUserData)->tracyContext =
//             TracyVkContext(
//                 myDeviceContext->getPhysicalDevice(),
//                 myDeviceContext->getDevice(),
//                 myDeviceContext->getPrimaryGraphicsQueue(),
//                 commands());
// }
}

template <>
void CommandContext<GraphicsBackend::Vulkan>::clear()
{
    ZoneScopedN("commandContextClear");

    {
        ZoneScopedN("pending");
        myPendingCommands.clear();
    }
    {
        ZoneScopedN("submitted");
        mySubmittedCommands.clear();
    }
    {
        ZoneScopedN("free");
        myFreeCommands.clear();        
    }

    myLastSubmitTimelineValue.reset();
    myScratchMemory.clear();
}

template <>
CommandContext<GraphicsBackend::Vulkan>::~CommandContext()
{
    ZoneScopedN("~CommandContext()");

    if (std::any_cast<command_vulkan::UserData>(&myUserData)->tracyContext)
        TracyVkDestroy(std::any_cast<command_vulkan::UserData>(&myUserData)->tracyContext);
}

template <>
template <>
command_vulkan::UserData& CommandContext<GraphicsBackend::Vulkan>::userData<command_vulkan::UserData>()
{
    return std::any_cast<command_vulkan::UserData&>(myUserData);
}
