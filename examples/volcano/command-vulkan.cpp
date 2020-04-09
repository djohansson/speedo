#include "command.h"

template <>
void CommandContext<GraphicsBackend::Vulkan>::begin() const
{
    VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    beginInfo.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    CHECK_VK(vkBeginCommandBuffer(myCommandBuffer, &beginInfo));
}

template <>
void CommandContext<GraphicsBackend::Vulkan>::submit() const
{   
    VkTimelineSemaphoreSubmitInfo timelineInfo = { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
    timelineInfo.pNext = NULL;
    timelineInfo.waitSemaphoreValueCount = 1;
    timelineInfo.pWaitSemaphoreValues = &myDesc.commandsBeginWaitValue;
    timelineInfo.signalSemaphoreValueCount = 1;
    timelineInfo.pSignalSemaphoreValues = &myDesc.commandsEndSignalValue;

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submitInfo.pNext = &timelineInfo;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &myDesc.timelineSemaphore;
    submitInfo.pWaitDstStageMask = &waitStage;
    submitInfo.signalSemaphoreCount  = 1;
    submitInfo.pSignalSemaphores = &myDesc.timelineSemaphore;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &myCommandBuffer;

    CHECK_VK(vkQueueSubmit(myDesc.queue, 1, &submitInfo, VK_NULL_HANDLE));
}

template <>
void CommandContext<GraphicsBackend::Vulkan>::end() const
{
    CHECK_VK(vkEndCommandBuffer(myCommandBuffer));
}

template <>
bool CommandContext<GraphicsBackend::Vulkan>::isComplete() const
{
    uint64_t value;
    CHECK_VK(vkGetSemaphoreCounterValue(myDesc.device, myDesc.timelineSemaphore, &value));

    return (value >= myDesc.commandsEndSignalValue);
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
        waitInfo.pValues = &myDesc.commandsEndSignalValue;

        CHECK_VK(vkWaitSemaphores(myDesc.device, &waitInfo, UINT64_MAX));
    }
}

template <>
CommandContext<GraphicsBackend::Vulkan>::CommandContext(CommandCreateDesc<GraphicsBackend::Vulkan>&& desc)
: myDesc(std::move(desc))
{
    VkCommandBufferAllocateInfo cmdInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cmdInfo.commandPool = myDesc.commandPool;
    cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdInfo.commandBufferCount = 1;
    CHECK_VK(vkAllocateCommandBuffers(myDesc.device, &cmdInfo, &myCommandBuffer));
}

template <>
CommandContext<GraphicsBackend::Vulkan>::~CommandContext()
{
    assert(isComplete());

    vkFreeCommandBuffers(myDesc.device, myDesc.commandPool, 1, &myCommandBuffer);
}
