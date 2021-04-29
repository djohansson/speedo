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

    if (getDesc().useBufferReset)
    {
        for (uint32_t i = 0ul; i < head(); i++)
        {
            ZoneScopedN("CommandBufferArray::reset::vkResetCommandBuffer");
            
            VK_CHECK(vkResetCommandBuffer(myArray[i], VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT));
        }
    }

    myBits = {0, 0};
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
void CommandPool<Vk>::swap(CommandPool& other) noexcept
{
	DeviceObject::swap(other);
	std::swap(myDesc, other.myDesc);
	std::swap(myPool, other.myPool);
}

template <>
void CommandPool<Vk>::reset()
{
    ZoneScopedN("CommandPool::reset");

    if (getDesc().usePoolReset)
    {
        ZoneScopedN("CommandPool::reset::vkResetCommandPool");

        VK_CHECK(vkResetCommandPool(getDeviceContext()->getDevice(), myPool, VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT));
    }
}

template <>
void CommandPoolContext<Vk>::internalEnqueueOnePending(CommandBufferLevel<Vk> level)
{
    ZoneScopedN("CommandPoolContext::internalEnqueueOnePending");

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
                0));
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
        internalEnqueueOnePending(beginInfo.level);

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
void CommandPoolContext<Vk>::addCommandsFinishedCallback(std::function<void(uint64_t)>&& callback)
{
    myCommandsFinishedCallbacks.emplace_back(std::make_tuple(0, std::move(callback)));
}

template <>
void CommandPoolContext<Vk>::internalEnqueueSubmitted(CommandBufferListType&& commands, CommandBufferLevel<Vk> level, uint64_t timelineValue)
{
    ZoneScopedN("CommandPoolContext::internalEnqueueSubmitted");

    for (auto& [cmdArray, cmdTimelineValue] : commands)
        cmdTimelineValue = timelineValue;

    mySubmittedCommands[level].splice(mySubmittedCommands[level].end(), std::move(commands));

    while (!myCommandsFinishedCallbacks.empty())
    {
        auto& callback = myCommandsFinishedCallbacks.front();
        std::get<0>(callback) = timelineValue;
        getDeviceContext()->addTimelineCallback(std::move(callback));
        myCommandsFinishedCallbacks.pop_front();
    }
}

template <>
void CommandPoolContext<Vk>::reset()
{
    ZoneScopedN("CommandPoolContext::reset");

    CommandPool<Vk>::reset();

    for (uint32_t levelIt = 0ul; levelIt < kCommandBufferLevelCount; levelIt++)
    {
        auto& submittedCommandList = mySubmittedCommands[levelIt];

        for (auto& commands : submittedCommandList)
            std::get<0>(commands).reset();

        auto& freeCommandList = myFreeCommands[levelIt];

        freeCommandList.splice(freeCommandList.end(), std::move(submittedCommandList));
    }
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

    for (const auto& [cmdArray, cmdTimelineValue] : pendingCommands)
    {
        assert(!cmdArray.recordingFlags());
        auto cmdCount = cmdArray.head();
        std::copy_n(cmdArray.data(), cmdCount, std::back_inserter(submitInfo.commandBuffers));
    }

    const auto [minSignalValue, maxSignalValue] = std::minmax_element(
        submitInfo.signalSemaphoreValues.begin(),
        submitInfo.signalSemaphoreValues.end());

    submitInfo.timelineValue = *maxSignalValue;

    internalEnqueueSubmitted(std::move(pendingCommands), VK_COMMAND_BUFFER_LEVEL_PRIMARY, *maxSignalValue);
    
    return submitInfo;
}

template <>
uint64_t CommandPoolContext<Vk>::execute(CommandPoolContext<Vk>& callee)
{
    ZoneScopedN("CommandPoolContext::execute");

    callee.internalEndCommands(VK_COMMAND_BUFFER_LEVEL_SECONDARY);

    for (const auto& [cmdArray, cmdTimelineValue] : callee.myPendingCommands[VK_COMMAND_BUFFER_LEVEL_SECONDARY])
        vkCmdExecuteCommands(commands(), cmdArray.head(), cmdArray.data());

    auto timelineValue = getDeviceContext()->timelineValue().load(std::memory_order_relaxed);

	callee.internalEnqueueSubmitted(
        std::move(callee.myPendingCommands[VK_COMMAND_BUFFER_LEVEL_SECONDARY]),
        VK_COMMAND_BUFFER_LEVEL_SECONDARY,
        timelineValue);

    return timelineValue;
}

template <>
CommandPoolContext<Vk>::CommandPoolContext(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    CommandPoolCreateDesc<Vk>&& poolDesc)
: CommandPool(deviceContext, std::move(poolDesc))
{
    ZoneScopedN("CommandPoolContext()");
}

template <>
CommandPoolContext<Vk>::CommandPoolContext(CommandPoolContext&& other) noexcept
: CommandPool(std::move(other))
, myPendingCommands(std::exchange(other.myPendingCommands, {}))
, mySubmittedCommands(std::exchange(other.mySubmittedCommands, {}))
, myFreeCommands(std::exchange(other.myFreeCommands, {}))
, myRecordingCommands(std::exchange(other.myRecordingCommands, {}))
, myCommandsFinishedCallbacks(std::exchange(other.myCommandsFinishedCallbacks, {}))
{
}

template <>
CommandPoolContext<Vk>::~CommandPoolContext()
{
    ZoneScopedN("~CommandPoolContext()");

    for (auto& submittedCommandList : mySubmittedCommands)
    {
        if (!submittedCommandList.empty())
        {
            const auto& [cmdArray, cmdTimelineValue] = submittedCommandList.back();

            getDeviceContext()->wait(cmdTimelineValue);
            getDeviceContext()->processTimelineCallbacks(cmdTimelineValue);
        }
    }
}

template <>
CommandPoolContext<Vk>& CommandPoolContext<Vk>::operator=(CommandPoolContext&& other) noexcept
{
    CommandPool::operator=(std::move(other));
    myPendingCommands = std::exchange(other.myPendingCommands, {});
    mySubmittedCommands = std::exchange(other.mySubmittedCommands, {});
    myFreeCommands = std::exchange(other.myFreeCommands, {});
    myRecordingCommands = std::exchange(other.myRecordingCommands, {});
    myCommandsFinishedCallbacks = std::exchange(other.myCommandsFinishedCallbacks, {});
    return *this;
}

template <>
void CommandPoolContext<Vk>::swap(CommandPoolContext& other) noexcept
{
    CommandPool::swap(other);
    std::swap(myPendingCommands, other.myPendingCommands);
    std::swap(mySubmittedCommands, other.mySubmittedCommands);
    std::swap(myFreeCommands, other.myFreeCommands);
    std::swap(myRecordingCommands, other.myRecordingCommands);
    std::swap(myCommandsFinishedCallbacks, other.myCommandsFinishedCallbacks);
}
