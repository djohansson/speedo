#pragma once

#include "device.h"
#include "gfx-types.h"

#include <atomic>
#include <cassert>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

template <GraphicsBackend B>
struct CommandDesc
{
    std::shared_ptr<DeviceContext<B>> deviceContext;
    CommandPoolHandle<B> commandPool = 0; // perhaps redunant...
    uint32_t commandBufferLevel = 0; // perhaps redunant...
    SemaphoreHandle<B> timelineSemaphore = 0;
    std::shared_ptr<std::atomic_uint64_t> timelineValue;
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

    CommandContext(CommandContext<B>&& other)
    : myCommandDesc(std::move(other.myCommandDesc))
    , myCommandBuffer(other.myCommandBuffer)
    , myLastSubmitTimelineValue(other.myLastSubmitTimelineValue)
    {
        other.myCommandDesc = {};
        other.myCommandBuffer = 0;
        other.myLastSubmitTimelineValue = 0;
    }
    CommandContext(CommandDesc<B>&& desc);
    ~CommandContext();

    const auto& getCommandDesc() const { return myCommandDesc; }
    const auto& getCommandBuffer() const { return myCommandBuffer; }

    void begin(const CommandBufferBeginInfo<B>* beginInfo = nullptr) const;
    void end() const;
    uint64_t submit(const CommandSubmitInfo<B>& submitInfo = CommandSubmitInfo<B>());
    void sync(std::optional<uint64_t> timelineValue = std::nullopt);
    void free();

    template <typename T>
    uint64_t addSyncCallback(T callback)
    {
        uint64_t timelineValue = myCommandDesc.timelineValue->fetch_add(1, std::memory_order_relaxed);
        mySyncCallbacks.emplace_back(std::make_pair(callback, timelineValue));
        return timelineValue;
    }

private:

    bool hasReached(uint64_t timelineValue) const;

    CommandDesc<B> myCommandDesc = {};
    CommandBufferHandle<B> myCommandBuffer = 0; // todo: 1 -> many?
    std::vector<std::pair<std::function<void()>, uint64_t>> mySyncCallbacks;
    uint64_t myLastSubmitTimelineValue = 0;

    static thread_local std::vector<std::byte> threadScratchMemory;
};
