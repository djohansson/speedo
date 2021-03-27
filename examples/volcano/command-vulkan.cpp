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

    {
        ZoneScopedN("commandbufferarray::createArray::vkAllocateCommandBuffers");

        VkCommandBufferAllocateInfo cmdInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        cmdInfo.commandPool = desc.pool;
        cmdInfo.level = desc.level;
        cmdInfo.commandBufferCount = CommandBufferArray<Vk>::capacity();
        VK_CHECK(vkAllocateCommandBuffers(
            deviceContext->getDevice(),
            &cmdInfo,
            outArray.data()));
    }
    
    return outArray;
}

}

template <>
CommandBufferArray<Vk>::CommandBufferArray(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    std::tuple<
        CommandBufferArrayCreateDesc<Vk>,
        std::array<CommandBufferHandle<Vk>, kCommandBufferCount>>&& descAndData)
: DeviceObject(
    deviceContext,
    {"_CommandBufferArray"},
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
: DeviceObject(std::move(other))
, myDesc(std::exchange(other.myDesc, {}))
, myArray(std::exchange(other.myArray, {}))
, myBits(other.myBits)
{
}

template <>
CommandBufferArray<Vk>::~CommandBufferArray()
{
    ZoneScopedN("~CommandBufferArray()");

    if (isValid())
    {
        ZoneScopedN("~CommandBufferArray()::vkFreeCommandBuffers");

        vkFreeCommandBuffers(
            getDeviceContext()->getDevice(),
            myDesc.pool,
            kCommandBufferCount,
            myArray.data());
    }
}

template <>
CommandBufferArray<Vk>& CommandBufferArray<Vk>::operator=(CommandBufferArray&& other) noexcept
{
    DeviceObject::operator=(std::move(other));
    myDesc = std::exchange(other.myDesc, {});
    myArray = std::exchange(other.myArray, {});
    myBits = other.myBits;
    return *this;
}

template <>
void CommandBufferArray<Vk>::swap(CommandBufferArray& rhs) noexcept
{
    DeviceObject::swap(rhs);
    std::swap(myDesc, rhs.myDesc);
    std::swap(myArray, rhs.myArray);
    std::swap(myBits, rhs.myBits);
}

template <>
void CommandBufferArray<Vk>::reset()
{
    ZoneScopedN("CommandBufferArray::reset");

    assert(!recordingFlags());
    assert(head() < kCommandBufferCount);
    
    for (uint32_t i = 0ul; i < myBits.head; i++)
    {
        ZoneScopedN("CommandBufferArray::reset::vkResetCommandBuffer");
        
        VK_CHECK(vkResetCommandBuffer(myArray[i], VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT));
    }

    myBits = {0, 0};
}

template <>
CommandPool<Vk>::CommandPool(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    std::tuple<CommandPoolCreateDesc<Vk>, CommandPoolHandle<Vk>>&& descAndData)
: DeviceObject(
    deviceContext,
    {},
    1,
    VK_OBJECT_TYPE_COMMAND_POOL, 
    reinterpret_cast<uint64_t*>(&std::get<1>(descAndData)))
, myDesc(std::move(std::get<0>(descAndData)))
, myPool(std::move(std::get<1>(descAndData)))
{
}

template <>
CommandPool<Vk>::CommandPool(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    CommandPoolCreateDesc<Vk>&& desc)
: CommandPool(
    deviceContext,
    std::make_tuple(
        std::move(desc),
        createCommandPool(
            deviceContext->getDevice(),
            desc.flags,
            desc.queueFamilyIndex)))
{
}

template <>
CommandPool<Vk>::CommandPool(CommandPool&& other) noexcept
: DeviceObject(std::move(other))
, myDesc(std::exchange(other.myDesc, {}))
, myPool(std::exchange(other.myPool, {}))
{
}

template <>
CommandPool<Vk>::~CommandPool()
{
    if (myPool)
        vkDestroyCommandPool(getDeviceContext()->getDevice(), myPool, nullptr);
}

template <>
CommandPool<Vk>& CommandPool<Vk>::operator=(CommandPool&& other) noexcept
{
    DeviceObject::operator=(std::move(other));
    myDesc = std::exchange(other.myDesc, {});
    myPool = std::exchange(other.myPool, {});
    return *this;
}

template <>
void CommandPool<Vk>::swap(CommandPool& rhs) noexcept
{
    DeviceObject::swap(rhs);
    std::swap(myDesc, rhs.myDesc);
    std::swap(myPool, rhs.myPool);
}

template <>
void CommandPool<Vk>::reset()
{
    VK_CHECK(vkResetCommandPool(getDeviceContext()->getDevice(), myPool, 0));
}

template <>
void CommandPoolContext<Vk>::enqueueOnePending(CommandBufferLevel<Vk> level)
{
    ZoneScopedN("CommandPoolContext::enqueueOnePending");

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
                    getDeviceContext(),
                    CommandBufferArrayCreateDesc<Vk>{*this, level}),
                0,
                std::reference_wrapper(*this)));
    }
}

template <>
CommandBufferAccessScopeDesc<Vk>::CommandBufferAccessScopeDesc(bool scopedBeginEnd)
: CommandBufferBeginInfo<Vk>{
    VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    nullptr,
    VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    &inheritance}
, level(VK_COMMAND_BUFFER_LEVEL_PRIMARY)
, inheritance{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO}
, scopedBeginEnd(scopedBeginEnd)
{
}

template <>
CommandBufferAccessScopeDesc<Vk>::CommandBufferAccessScopeDesc(const CommandBufferAccessScopeDesc& other)
: CommandBufferBeginInfo<Vk>(other)
, level(other.level)
, inheritance(other.inheritance)
, scopedBeginEnd(other.scopedBeginEnd)
{
    pInheritanceInfo = &inheritance;
}

template <>
CommandBufferAccessScopeDesc<Vk>& CommandBufferAccessScopeDesc<Vk>::operator=(const CommandBufferAccessScopeDesc& other)
{
    *static_cast<CommandBufferBeginInfo<Vk>*>(this) = other;
    level = other.level;
    inheritance = other.inheritance;
    scopedBeginEnd = other.scopedBeginEnd;
    pInheritanceInfo = &inheritance;
    return *this;
}

template <>
bool CommandBufferAccessScopeDesc<Vk>::operator==(const CommandBufferAccessScopeDesc& other) const
{
    bool result = true;

    if (this != &other)
    {
        result = other.flags == flags && other.level == level && scopedBeginEnd == other.scopedBeginEnd;
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
CommandBufferAccessScope<Vk> CommandPoolContext<Vk>::internalBeginScope(
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
CommandBufferAccessScope<Vk> CommandPoolContext<Vk>::internalCommands(const CommandBufferAccessScopeDesc<Vk>& beginInfo) const
{
    return myRecordingCommands[beginInfo.level].value();
}

template <>
void CommandPoolContext<Vk>::enqueueExecuted(CommandBufferListType&& commands, uint64_t timelineValue)
{
    ZoneScopedN("CommandPoolContext::enqueueExecuted");

    for (auto& [cmdArray, cmdTimelineValue, cmdContextRef] : commands)
        cmdTimelineValue = timelineValue;

    myExecutedCommands.splice(myExecutedCommands.end(), std::move(commands));

    getDeviceContext()->addTimelineCallback(
        std::make_tuple(timelineValue, [this](uint64_t timelineValue)
        {
            ZoneScopedN("CommandPoolContext::cmdReset");

            auto onResetCommands = [](CommandBufferListType& from, uint64_t timelineValue)
            {
                auto fromBeginIt = from.begin();
                auto fromIt = fromBeginIt;

                while (fromIt != from.end())
                {
                    auto& [fromArray, fromTimelineValue, contextRef] = *fromIt;

                    if (fromTimelineValue >= timelineValue)
                        break;
                    
                    fromArray.reset();
                    
                    // fromIt++;
                    // from.pop_front();

                    auto& context = contextRef.get();
                    auto& to = context.myFreeCommands[fromArray.getDesc().level];
                    
                    to.splice(to.end(), from, fromIt++);
                }
            };

            onResetCommands(myExecutedCommands, timelineValue);
        }));
}

template <>
void CommandPoolContext<Vk>::addCommandsFinishedCallback(std::function<void(uint64_t)>&& callback)
{
    mySubmitFinishedCallbacks.emplace_back(std::make_tuple(0, std::move(callback)));
}

template <>
void CommandPoolContext<Vk>::enqueueSubmitted(CommandBufferListType&& commands, uint64_t timelineValue)
{
    ZoneScopedN("CommandPoolContext::enqueueSubmitted");

    for (auto& [cmdArray, cmdTimelineValue, cmdContextRef] : commands)
        cmdTimelineValue = timelineValue;

    mySubmittedCommands.splice(mySubmittedCommands.end(), std::move(commands));

    addCommandsFinishedCallback([this](uint64_t timelineValue)
    {
        ZoneScopedN("CommandPoolContext::cmdReset");

        auto onResetCommands = [](CommandBufferListType& from, CommandBufferListType& to, uint64_t timelineValue)
        {
            auto fromBeginIt = from.begin();
            auto fromIt = fromBeginIt;

            while (fromIt != from.end())
            {
                auto& [fromArray, fromTimelineValue, contextRef] = *fromIt;

                if (fromTimelineValue >= timelineValue)
                    break;

                fromArray.reset();
                fromIt++;
                // from.pop_front();
            }

            to.splice(to.end(), std::move(from), fromBeginIt, fromIt);
        };

        onResetCommands(
            mySubmittedCommands,
            myFreeCommands[VK_COMMAND_BUFFER_LEVEL_PRIMARY],
            timelineValue);
    });

    for (auto& callback : mySubmitFinishedCallbacks)
        std::get<0>(callback) = timelineValue;

    getDeviceContext()->addTimelineCallbacks(mySubmitFinishedCallbacks);
    mySubmitFinishedCallbacks.clear();
}

template <>
QueueSubmitInfo<Vk> CommandPoolContext<Vk>::prepareSubmit(QueueSyncInfo<Vk>&& syncInfo)
{
    ZoneScopedN("CommandPoolContext::prepareSubmit");

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
uint64_t CommandPoolContext<Vk>::execute(CommandPoolContext<Vk>& callee)
{
    ZoneScopedN("CommandPoolContext::execute");

    callee.internalEndCommands(VK_COMMAND_BUFFER_LEVEL_SECONDARY);

    for (const auto& [cmdArray, cmdTimelineValue, cmdContextRef] : callee.myPendingCommands[VK_COMMAND_BUFFER_LEVEL_SECONDARY])
        vkCmdExecuteCommands(commands(), cmdArray.head(), cmdArray.data());

    auto timelineValue = getDeviceContext()->timelineValue().load(std::memory_order_relaxed);

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
CommandPoolContext<Vk>::CommandPoolContext(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    CommandPoolCreateDesc<Vk>&& poolDesc)
: CommandPool(deviceContext, std::move(poolDesc))
, myPendingCommands(2)
, myFreeCommands(2)
, myRecordingCommands(2)
{
    ZoneScopedN("CommandPoolContext()");
}

template <>
CommandPoolContext<Vk>::CommandPoolContext(CommandPoolContext&& other) noexcept
: CommandPool(std::move(other))
, myPendingCommands(std::exchange(other.myPendingCommands, {}))
, myExecutedCommands(std::exchange(other.myExecutedCommands, {}))
, mySubmittedCommands(std::exchange(other.mySubmittedCommands, {}))
, myFreeCommands(std::exchange(other.myFreeCommands, {}))
, myRecordingCommands(std::exchange(other.myRecordingCommands, {}))
, mySubmitFinishedCallbacks(std::exchange(other.mySubmitFinishedCallbacks, {}))
{
}

template <>
CommandPoolContext<Vk>::~CommandPoolContext()
{
    ZoneScopedN("~CommandPoolContext()");

    if (!mySubmittedCommands.empty())
    {
        const auto& [cmdArray, cmdTimelineValue, cmdContextRef] = mySubmittedCommands.back();

        getDeviceContext()->wait(cmdTimelineValue);
        getDeviceContext()->processTimelineCallbacks(cmdTimelineValue);
    }
}

template <>
CommandPoolContext<Vk>& CommandPoolContext<Vk>::operator=(CommandPoolContext&& other) noexcept
{
    CommandPool::operator=(std::move(other));
    myPendingCommands = std::exchange(other.myPendingCommands, {});
    myExecutedCommands = std::exchange(other.myExecutedCommands, {});
    mySubmittedCommands = std::exchange(other.mySubmittedCommands, {});
    myFreeCommands = std::exchange(other.myFreeCommands, {});
    myRecordingCommands = std::exchange(other.myRecordingCommands, {});
    mySubmitFinishedCallbacks = std::exchange(other.mySubmitFinishedCallbacks, {});
    return *this;
}

template <>
void CommandPoolContext<Vk>::swap(CommandPoolContext& other) noexcept
{
    CommandPool::swap(other);
    std::swap(myPendingCommands, other.myPendingCommands);
    std::swap(myExecutedCommands, other.myExecutedCommands);
    std::swap(mySubmittedCommands, other.mySubmittedCommands);
    std::swap(myFreeCommands, other.myFreeCommands);
    std::swap(myRecordingCommands, other.myRecordingCommands);
    std::swap(mySubmitFinishedCallbacks, other.mySubmitFinishedCallbacks);
}
