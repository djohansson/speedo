#pragma once

#include "device.h"
#include "types.h"

template <GraphicsBackend B>
struct QueueCreateDesc : DeviceResourceCreateDesc<B>
{
    QueueHandle<B> queue;
};

template <GraphicsBackend B>
class Queue : public DeviceResource<B>
{
public:

    Queue(Queue<B>&& other) = default;
    Queue(
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        QueueCreateDesc<B>&& desc);

    Queue& operator=(Queue&& other) = default;

    auto getQueue() const { return myDesc.queue; }

    void submit(const SubmitInfo<B>* submits, uint32_t submitCount, FenceHandle<B> fence = {}) const;
    void waitIdle() const;

private:

    const QueueCreateDesc<B> myDesc = {};
};
