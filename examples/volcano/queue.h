#pragma once

#include "device.h"
#include "types.h"

#include <any>
#include <memory>
//#include <source_location>
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
    QueueHandle<B> queue = {};
    bool tracingEnabled = false;
};

struct SourceLocationData
{
    const char* name = nullptr;
    const char* function = nullptr;
    const char* file = nullptr;
    uint32_t line = 0;
    uint32_t color = 0;
};

template <GraphicsBackend B>
class Queue : public DeviceResource<B>
{
public:

    Queue(
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        QueueCreateDesc<B>&& desc);
    Queue(Queue<B>&& other);
    ~Queue();

    Queue& operator=(Queue&& other);
    operator auto() const { return myDesc.queue; }

    template <typename... Args>
    void enqueue(Args&&... args) { myPendingSubmits.emplace_back(std::move(args)...); }
    void collect(CommandBufferHandle<B> cmd);
    template <typename Function>
    std::shared_ptr<void> trace(CommandBufferHandle<B> cmd, const char* name, const char* function, const char* file, uint32_t line);
    uint64_t submit();
    void waitIdle() const;

private:

    std::shared_ptr<void> internalTrace(CommandBufferHandle<B> cmd, const SourceLocationData& srcLoc);

    QueueCreateDesc<B> myDesc = {};
    std::vector<QueueSubmitInfo<B>> myPendingSubmits;
    std::vector<std::byte> myScratchMemory;
    FenceHandle<B> myFence = {};
    std::any myUserData;
};

#include "queue-vulkan.inl"
