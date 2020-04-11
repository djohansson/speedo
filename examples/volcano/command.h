#pragma once

#include "gfx-types.h"

#include <atomic>
#include <vector>

template <GraphicsBackend B>
struct CommandCreateDesc
{
    DeviceHandle<B> device = 0;
    QueueHandle<B> queue = 0;
    CommandPoolHandle<B> commandPool = 0;
    SemaphoreHandle<B> timelineSemaphore = 0;
    std::shared_ptr<std::atomic_uint64_t> timelineValue;
    uint64_t commandBufferLevel = 0;
};

template <GraphicsBackend B>
struct CommandSubmitInfo
{
    uint32_t waitSemaphoreCount = 0;
    const SemaphoreHandle<B>* waitSemaphores = nullptr;
    const Flags<B>* waitDstStageMasks = 0;
    uint32_t signalSemaphoreCount = 0;
    const SemaphoreHandle<B>* signalSemaphores = nullptr;
    FenceHandle<B> signalFence = 0;
};

template <GraphicsBackend B>
class CommandContext : Noncopyable
{
public:

    CommandContext() = default;
    CommandContext(CommandContext<B>&& other)
    : myDesc(other.myDesc)
    , myCommandBuffer(other.myCommandBuffer)
    , myLastSubmitTimelineValue(other.myLastSubmitTimelineValue)
    {
        other.myDesc = {};
        other.myCommandBuffer = 0;
        other.myLastSubmitTimelineValue = 0;
    }
    CommandContext(CommandCreateDesc<B>&& desc);
    ~CommandContext();
    
    const auto getCommandBuffer() const { return myCommandBuffer; }

    bool isComplete() const;

    void begin(const CommandBufferBeginInfo<B>* beginInfo = nullptr) const;
    void submit(const CommandSubmitInfo<B>& submitInfo = CommandSubmitInfo<B>());
    void end() const;
    void sync() const;
    void free();

private:

    CommandCreateDesc<B> myDesc = {};
    CommandBufferHandle<B> myCommandBuffer = 0;
    uint64_t myLastSubmitTimelineValue = 0;

    static thread_local std::vector<std::byte> threadScratchMemory;
};
