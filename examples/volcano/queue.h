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
struct QueueSubmitInfo : QueueSyncInfo<B>
{
    std::vector<CommandBufferHandle<B>> commandBuffers;
    uint64_t timelineValue = 0;
};

template <GraphicsBackend B>
struct QueuePresentInfo
{
    std::vector<SemaphoreHandle<B>> waitSemaphores;
    std::vector<SwapchainHandle<B>> swapchains;
    std::vector<uint32_t> imageIndices;
    std::vector<Result<B>> results;
    uint64_t timelineValue = 0;

    QueuePresentInfo<B>& operator^=(QueuePresentInfo<B>&& other)
    {
        waitSemaphores.insert(
            waitSemaphores.end(),
            std::make_move_iterator(other.waitSemaphores.begin()),
            std::make_move_iterator(other.waitSemaphores.end()));
        other.waitSemaphores.clear();
        swapchains.insert(
            swapchains.end(),
            std::make_move_iterator(other.swapchains.begin()),
            std::make_move_iterator(other.swapchains.end()));
        other.swapchains.clear();
        imageIndices.insert(
            imageIndices.end(),
            std::make_move_iterator(other.imageIndices.begin()),
            std::make_move_iterator(other.imageIndices.end()));
        other.imageIndices.clear();
        results.insert(
            results.end(),
            std::make_move_iterator(other.results.begin()),
            std::make_move_iterator(other.results.end()));
        other.results.clear();
        timelineValue = std::max(timelineValue, std::exchange(other.timelineValue, 0));
        return *this;
    }
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

    template <typename... Ts>
    void enqueueSubmit(Ts&&... args);
    uint64_t submit();
    
    template <typename T, typename... Ts>
    void enqueuePresent(T&& first, Ts&&... rest);
    void present();
    
    void waitIdle() const;

    template <typename Function>
    std::shared_ptr<void> trace(CommandBufferHandle<B> cmd, const char* name, const char* function, const char* file, uint32_t line);

    void collectTracing(CommandBufferHandle<B> cmd);

private:

    std::shared_ptr<void> internalTrace(CommandBufferHandle<B> cmd, const SourceLocationData& srcLoc);

    QueueCreateDesc<B> myDesc = {};
    std::vector<QueueSubmitInfo<B>> myPendingSubmits;
    QueuePresentInfo<B> myPendingPresent = {};
    std::vector<std::byte> myScratchMemory;
    FenceHandle<B> myFence = {};
    std::any myUserData;
};

#include "queue-vulkan.inl"
