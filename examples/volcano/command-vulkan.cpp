#include "command.h"
#include "vk-utils.h"

#include <stb_sprintf.h>

namespace commandbufferarray
{

static auto createArray(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    const CommandBufferArrayCreateDesc<Vk>& desc)
{
    ZoneScopedN("commandbufferarray::createArray");

    std::array<CommandBufferHandle<Vk>, CommandBufferArray<Vk>::capacity()> outArray;

    VkCommandBufferAllocateInfo cmdInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cmdInfo.commandPool = desc.pool;
    cmdInfo.level = desc.level;
    cmdInfo.commandBufferCount = CommandBufferArray<Vk>::capacity();
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
: CommandBufferArray(
    deviceContext,
    std::make_tuple(
        std::move(desc),
        commandbufferarray::createArray(
            deviceContext,
            desc)))
{
}

template <>
CommandBufferArray<Vk>::CommandBufferArray(CommandBufferArray&& other) noexcept
: DeviceResource(std::move(other))
, myDesc(std::exchange(other.myDesc, {}))
, myArray(std::exchange(other.myArray, {}))
, myBits(other.myBits)
{
}

template <>
CommandBufferArray<Vk>::~CommandBufferArray()
{
    ZoneScopedN("~CommandBufferArray()");

    if (isValid()) // todo: put on timeline?
        vkFreeCommandBuffers(
            getDeviceContext()->getDevice(),
            myDesc.pool,
            kCommandBufferCount,
            myArray.data());
}

template <>
CommandBufferArray<Vk>& CommandBufferArray<Vk>::operator=(CommandBufferArray&& other) noexcept
{
    DeviceResource::operator=(std::move(other));
    myDesc = std::exchange(other.myDesc, {});
    myArray = std::exchange(other.myArray, {});
    myBits = other.myBits;
    return *this;
}

template <>
void CommandBufferArray<Vk>::swap(CommandBufferArray& rhs) noexcept
{
    DeviceResource::swap(rhs);
    std::swap(myDesc, rhs.myDesc);
    std::swap(myArray, rhs.myArray);
    std::swap(myBits, rhs.myBits);
}

template <>
void CommandBufferArray<Vk>::resetAll()
{
    ZoneScopedN("CommandBufferArray::resetAll");

    assert(!recordingFlags());
    assert(head() < kCommandBufferCount);
    
    for (uint32_t i = 0; i < myBits.head; i++)
        VK_CHECK(vkResetCommandBuffer(myArray[i], VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT));

    myBits = {0, 0};
}

template <>
void CommandContext<Vk>::enqueueOnePending(CommandBufferLevel<Vk> level)
{
    ZoneScopedN("CommandContext::enqueueOnePending");

    if (!myFreeCommands[level].empty())
    {
        myPendingCommands[level].splice(
            myPendingCommands[level].end(),
            myFreeCommands[level],
            myFreeCommands[level].begin());
    }
    else
    {
        char stringBuffer[32];

        stbsp_sprintf(
            stringBuffer,
            "%s%s",
            level == VK_COMMAND_BUFFER_LEVEL_PRIMARY ? "Primary" : "Secondary",
            "CommandBufferArray");
            
        myPendingCommands[level].emplace_back(
            std::make_tuple(
                CommandBufferArray<Vk>(
                    myDevice,
                    CommandBufferArrayCreateDesc<Vk>{
                        {stringBuffer},
                        myDesc.pool,
                        level}),
                0,
                std::reference_wrapper(*this)));
    }
}

template <>
CommandBufferAccessScopeDesc<Vk>::CommandBufferAccessScopeDesc()
: CommandBufferBeginInfo<Vk>{
    VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    nullptr,
    VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    &inheritance}
, level(VK_COMMAND_BUFFER_LEVEL_PRIMARY)
, inheritance{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO}
{
}

template <>
CommandBufferAccessScopeDesc<Vk>::CommandBufferAccessScopeDesc(const CommandBufferAccessScopeDesc& other)
: CommandBufferBeginInfo<Vk>(other)
, level(other.level)
, inheritance(other.inheritance)
{
    pInheritanceInfo = &inheritance;
}

template <>
CommandBufferAccessScopeDesc<Vk>& CommandBufferAccessScopeDesc<Vk>::operator=(const CommandBufferAccessScopeDesc& other)
{
    *static_cast<CommandBufferBeginInfo<Vk>*>(this) = other;
    level = other.level;
    inheritance = other.inheritance;
    pInheritanceInfo = &inheritance;
    return *this;
}

template <>
bool CommandBufferAccessScopeDesc<Vk>::operator==(const CommandBufferAccessScopeDesc& other) const
{
    bool result = true;

    if (this != &other)
    {
        result = other.flags == flags && other.level == level;
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
    }

    return result;
}

template <>
CommandBufferAccessScope<Vk> CommandContext<Vk>::internalBeginScope(
    const CommandBufferAccessScopeDesc<Vk>& beginInfo)
{
    if (myPendingCommands[beginInfo.level].empty() || std::get<0>(myPendingCommands[beginInfo.level].back()).full())
        enqueueOnePending(beginInfo.level);

    myRecordingCommands[beginInfo.level].emplace(
        CommandBufferAccessScope(
            &std::get<0>(myPendingCommands[beginInfo.level].back()),
            beginInfo));

    return myRecordingCommands[beginInfo.level].value();
}
    
template <>
CommandBufferAccessScope<Vk> CommandContext<Vk>::internalCommands(const CommandBufferAccessScopeDesc<Vk>& beginInfo) const
{
    return myRecordingCommands[beginInfo.level].value();
}

template <>
void CommandContext<Vk>::enqueueExecuted(CommandBufferListType&& commands, uint64_t timelineValue)
{
    ZoneScopedN("CommandContext::enqueueExecuted");

    for (auto& [cmdArray, cmdTimelineValue, cmdContext] : commands)
        cmdTimelineValue = timelineValue;

    myExecutedCommands.splice(myExecutedCommands.end(), std::move(commands));

    myDevice->addTimelineCallback(timelineValue, [this](uint64_t timelineValue)
    {
        ZoneScopedN("CommandContext::cmdReset");

        auto onResetCommands = [](CommandBufferListType& from, uint64_t timelineValue)
        {
            auto fromBeginIt = from.begin();
            auto fromIt = fromBeginIt;

            while (fromIt != from.end())
            {
                auto& [fromArray, fromTimelineValue, contextRef] = *fromIt;

                if (fromTimelineValue >= timelineValue)
                    break;
                
                fromArray.resetAll();

                auto& context = contextRef.get();
                auto& to = context.myFreeCommands[fromArray.getDesc().level];           
                
                to.splice(to.end(), from, fromIt++);
            }
        };

        onResetCommands(myExecutedCommands, timelineValue);
    });
}

template <>
void CommandContext<Vk>::addCommandsFinishedCallback(std::function<void(uint64_t)>&& callback)
{
    mySubmitFinishedCallbacks.emplace_back(std::move(callback));
}

template <>
void CommandContext<Vk>::enqueueSubmitted(CommandBufferListType&& commands, uint64_t timelineValue)
{
    ZoneScopedN("CommandContext::enqueueSubmitted");

    for (auto& [cmdArray, cmdTimelineValue, cmdContext] : commands)
        cmdTimelineValue = timelineValue;

    mySubmittedCommands.splice(mySubmittedCommands.end(), std::move(commands));

    addCommandsFinishedCallback([this](uint64_t timelineValue)
    {
        ZoneScopedN("CommandContext::cmdReset");

        auto onResetCommands = [](CommandBufferListType& from, CommandBufferListType& to, uint64_t timelineValue)
        {
            auto fromBeginIt = from.begin();
            auto fromIt = fromBeginIt;

            while (fromIt != from.end())
            {
                auto& [fromArray, fromTimelineValue, contextRef] = *fromIt;

                if (fromTimelineValue >= timelineValue)
                    break;

                fromArray.resetAll();

                fromIt++;
            }

            to.splice(to.end(), std::move(from), fromBeginIt, fromIt);
        };

        onResetCommands(mySubmittedCommands, myFreeCommands[VK_COMMAND_BUFFER_LEVEL_PRIMARY], timelineValue);
    });

    myDevice->addTimelineCallbacks(timelineValue, mySubmitFinishedCallbacks);
    mySubmitFinishedCallbacks.clear();
}

template <>
QueueSubmitInfo<Vk> CommandContext<Vk>::prepareSubmit(QueueSyncInfo<Vk>&& syncInfo)
{
    ZoneScopedN("CommandContext::prepareSubmit");

    internalEndCommands(VK_COMMAND_BUFFER_LEVEL_PRIMARY);

    auto& pendingCommands = myPendingCommands[VK_COMMAND_BUFFER_LEVEL_PRIMARY];

    if (pendingCommands.empty())
        return {};

    QueueSubmitInfo<Vk> submitInfo{std::move(syncInfo), {}, 0};
    submitInfo.commandBuffers.reserve(pendingCommands.size() * CommandBufferArray<Vk>::capacity());

    for (const auto& [cmdArray, cmdTimelineValue, cmdContextRef] : pendingCommands)
    {
        assert(!cmdArray.recordingFlags());
        auto cmdCount = cmdArray.head();
        std::copy_n(cmdArray.data(), cmdCount, std::back_inserter(submitInfo.commandBuffers));
    }

    const auto [minSignalValue, maxSignalValue] = std::minmax_element(
        submitInfo.signalSemaphoreValues.begin(),
        submitInfo.signalSemaphoreValues.end());

    submitInfo.timelineValue = *maxSignalValue;

    enqueueSubmitted(std::move(pendingCommands), *maxSignalValue);
    
    return submitInfo;
}

template <>
uint64_t CommandContext<Vk>::execute(CommandContext<Vk>& callee)
{
    ZoneScopedN("CommandContext::execute");

    callee.internalEndCommands(VK_COMMAND_BUFFER_LEVEL_SECONDARY);

    for (const auto& [cmdArray, cmdTimelineValue, cmdContextRef] : callee.myPendingCommands[VK_COMMAND_BUFFER_LEVEL_SECONDARY])
        vkCmdExecuteCommands(commands(), cmdArray.head(), cmdArray.data());

    auto timelineValue = myDevice->timelineValue().load(std::memory_order_relaxed);

	enqueueExecuted(std::move(callee.myPendingCommands[VK_COMMAND_BUFFER_LEVEL_SECONDARY]), timelineValue);

    return timelineValue;
}

template <>
uint8_t CommandBufferArray<Vk>::begin(
    const CommandBufferBeginInfo<Vk>& beginInfo)
{
    ZoneScopedN("CommandBufferArray::begin");

    assert(!recording(myBits.head));
    assert(!full());
    
    VK_CHECK(vkBeginCommandBuffer(myArray[myBits.head], &beginInfo));

    myBits.recordingFlags |= (1 << myBits.head);

    return static_cast<uint8_t>(myBits.head++);
}

template <>
void CommandBufferArray<Vk>::end(uint8_t index)
{
    ZoneScopedN("CommandBufferArray::end");

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
, myRecordingCommands(2)
{
    ZoneScopedN("CommandContext()");
}

template <>
CommandContext<Vk>::~CommandContext()
{
    ZoneScopedN("~CommandContext()");

    if (!mySubmittedCommands.empty())
    {
        const auto& [cmdArray, cmdTimelineValue, cmdContextRef] = mySubmittedCommands.back();

        myDevice->wait(cmdTimelineValue);
        myDevice->processTimelineCallbacks(cmdTimelineValue);
    }
}
