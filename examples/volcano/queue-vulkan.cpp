#include "queue.h"
#include "vk-utils.h"

template <>
Queue<Vk>::Queue(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    QueueCreateDesc<Vk>&& desc)
: DeviceResource<Vk>(
    deviceContext,
    {"_Queue"},
    1,
    VK_OBJECT_TYPE_QUEUE,
    reinterpret_cast<uint64_t*>(&desc.queue))
, myDesc(std::move(desc))
{
}

template <>
void Queue<Vk>::submit(const SubmitInfo<Vk>* submits, uint32_t submitCount, FenceHandle<Vk> fence) const
{
    VK_CHECK(vkQueueSubmit(myDesc.queue, submitCount, submits, fence));
}

template <>
void Queue<Vk>::waitIdle() const
{
    VK_CHECK(vkQueueWaitIdle(myDesc.queue));
}
