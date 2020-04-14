#pragma once

#include "device.h"
#include "gfx-types.h"

#include <atomic>
#include <functional>
#include <memory>
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
    : myDesc(other.myDesc)
    , myCommandBuffer(other.myCommandBuffer)
    , myLastSubmitTimelineValue(other.myLastSubmitTimelineValue)
    {
        other.myDesc = {};
        other.myCommandBuffer = 0;
        other.myLastSubmitTimelineValue = 0;
    }
    CommandContext(CommandDesc<B>&& desc);
    ~CommandContext();

    const auto& getCommandDesc() const { return myDesc; }
    const auto& getCommandBuffer() const { return myCommandBuffer; }

    void begin(const CommandBufferBeginInfo<B>* beginInfo = nullptr) const;
    void end() const;
    void submit(const CommandSubmitInfo<B>& submitInfo = CommandSubmitInfo<B>());
    void sync();
    void free();

    template <typename T>
    void addSyncCallback(T callback) { mySyncCallbacks.emplace_back(callback); }

    static CommandContext<B> createTransferCommands(const std::shared_ptr<DeviceContext<B>>& deviceContext);

private:

    bool isComplete() const;

    CommandDesc<B> myDesc = {};
    CommandBufferHandle<B> myCommandBuffer = 0; // todo: 1 -> many?
    uint64_t myLastSubmitTimelineValue = 0;
    std::vector<std::function<void()>> mySyncCallbacks;

    static thread_local std::vector<std::byte> threadScratchMemory;
};
