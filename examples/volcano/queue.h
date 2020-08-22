#pragma once

#include "device.h"
#include "types.h"

#include <vector>

template <GraphicsBackend B>
struct QueueSyncInfo
{
    std::vector<SemaphoreHandle<B>> waitSemaphores;
    std::vector<Flags<B>> waitDstStageMasks;
    std::vector<uint64_t> waitSemaphoreValues;
    std::vector<SemaphoreHandle<B>> signalSemaphores;
    std::vector<uint64_t> signalSemaphoreValues;
};

template <GraphicsBackend B>
struct QueueSubmitInfo
{
    QueueSyncInfo<B> syncInfo;
    std::vector<CommandBufferHandle<B>> commandBuffers;
    uint64_t maxTimelineValue = 0;
};

template <GraphicsBackend B>
struct QueueCreateDesc : DeviceResourceCreateDesc<B>
{
    QueueHandle<B> queue;
};

template <GraphicsBackend B>
class Queue : public DeviceResource<B>
{
public:

    Queue(Queue<B>&& other);
    Queue(
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        QueueCreateDesc<B>&& desc);

    Queue& operator=(Queue&& other);
    operator auto() const { return myDesc.queue; }

    template <typename... Args>
    void enqueue(Args&&... args) { myPendingSubmits.emplace_back(std::move(args)...); }
    uint64_t submit();
    void waitIdle() const;

private:

    const QueueCreateDesc<B> myDesc = {};
    std::vector<QueueSubmitInfo<B>> myPendingSubmits;
    std::vector<std::byte> myScratchMemory;
    FenceHandle<B> myFence = {};
};
