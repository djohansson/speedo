#pragma once

#include "device.h"
#include "gfx-types.h"

#include <any>
#include <atomic>
#include <cassert>
#include <functional>
#include <list>
#include <memory>
#include <utility>
#include <vector>

template <GraphicsBackend B>
class CommandContext;

template <GraphicsBackend B, bool EndOnDestruct>
class CommandBufferAccessScope;

template <GraphicsBackend B>
struct CommandBufferArrayCreateDesc
{
    std::shared_ptr<CommandContext<B>> commandContext;
    CommandPoolHandle<B> commandPool = 0;
    uint32_t commandBufferLevel = 0;
};

template <GraphicsBackend B>
class CommandBufferArray
{
public:

    CommandBufferArray(CommandBufferArray<B>&& other) = default;
    CommandBufferArray(CommandBufferArrayCreateDesc<B>&& desc);
    ~CommandBufferArray();

    static uint32_t ourDebugCount;

private:

    friend class CommandContext<B>;
    friend class CommandBufferAccessScope<B, true>;
    friend class CommandBufferAccessScope<B, false>;

    void begin(const CommandBufferBeginInfo<B>* beginInfo = nullptr);
    bool end();
    void reset();

    const CommandBufferHandle<B>* data() const { assert(!recording()); return myCommandBufferArray; }
    uint8_t size() const { static_assert(kCommandBufferCount < 128); assert(!recording()); return myBits.myHead; }
    bool recording() const { return myBits.myRecording; }
    bool full() const { return (size() + 1) >= capacity(); }
    constexpr auto capacity() const { return kCommandBufferCount; }
    
    operator CommandBufferHandle<B>() const { return myCommandBufferArray[myBits.myHead]; }

    static constexpr uint32_t kCommandBufferCount = 8;

    CommandBufferArrayCreateDesc<B> myDesc = {};
    CommandBufferHandle<B> myCommandBufferArray[kCommandBufferCount] = { static_cast<CommandBufferHandle<B>>(0) };
    struct Bits
    {
        uint8_t myHead : 7;
        uint8_t myRecording : 1;
    } myBits = {};
};

template <GraphicsBackend B, bool CallBeginAndEnd>
class CommandBufferAccessScope : Noncopyable, Nondynamic
{
public:

    CommandBufferAccessScope(CommandBufferArray<B>& array, const CommandBufferBeginInfo<B>* beginInfo = nullptr)
        : myArray(array)
    {
        if constexpr (CallBeginAndEnd)
            myArray.begin(beginInfo);
    }

    ~CommandBufferAccessScope()
    {
        if constexpr (CallBeginAndEnd)
            myArray.end();
    }

    operator CommandBufferHandle<B>() const { return myArray; }

private:

    CommandBufferArray<B>& myArray;
};

template <GraphicsBackend B>
struct CommandContextCreateDesc
{
    std::shared_ptr<DeviceContext<B>> deviceContext;
    CommandPoolHandle<B> commandPool = 0;
    uint32_t commandBufferLevel = 0;
    SemaphoreHandle<B> timelineSemaphore = 0;
    std::shared_ptr<std::atomic_uint64_t> timelineValue;
};

template <GraphicsBackend B>
struct CommandSubmitInfo
{
    QueueHandle<B> queue = 0;
    FenceHandle<B> signalFence = 0;
    uint32_t signalSemaphoreCount = 0;
    const SemaphoreHandle<B>* signalSemaphores = nullptr;
    uint32_t waitSemaphoreCount = 0;
    const SemaphoreHandle<B>* waitSemaphores = nullptr;
    const Flags<B>* waitDstStageMasks = 0;
    const uint64_t* waitSemaphoreValues = 0;
};

template <GraphicsBackend B>
class CommandContext : public std::enable_shared_from_this<CommandContext<B>>
{
public:

    CommandContext(CommandContext<B>&& other) = default;
    CommandContext(CommandContextCreateDesc<B>&& desc);
    ~CommandContext();

    static uint32_t ourDebugCount;

    const auto& getDesc() const { return myDesc; }
    const auto& getLastSubmitTimelineValue() const { return myLastSubmitTimelineValue; }

    auto beginEndScope(const CommandBufferBeginInfo<B>* beginInfo = nullptr)
    {
        if (myPendingCommands.empty() || myPendingCommands.back().full())
            enqueueOnePending();

        return CommandBufferAccessScope<B, true>(myPendingCommands.back(), beginInfo);
    }
    
    auto commands()
    {
        if (myPendingCommands.empty())
            enqueueOnePending();

        return CommandBufferAccessScope<B, false>(myPendingCommands.back());
    }

    bool isPendingEmpty() const { return myPendingCommands.empty(); }
    bool isSubmittedEmpty() const { return mySubmittedCommands.empty(); }

    uint64_t execute(CommandContext<B>& other, const RenderPassBeginInfo<B>* beginInfo);
    uint64_t submit(const CommandSubmitInfo<B>& submitInfo = CommandSubmitInfo<B>());

    bool hasReached(uint64_t timelineValue) const;
    void wait(uint64_t timelineValue) const;

    void collectGarbage(
        std::optional<uint64_t> waitTimelineValue = std::nullopt,
        std::optional<FenceHandle<B>> waitFence = std::nullopt);

    template <typename T>
    uint64_t addGarbageCollectCallback(T callback, uint64_t timelineValue)
    {
        myGarbageCollectCallbacks.emplace_back(std::make_pair(callback, timelineValue));

        return timelineValue;
    }

    template <typename T>
    uint64_t addGarbageCollectCallback(T callback)
    {
        return addGarbageCollectCallback(callback,
            myDesc.timelineValue->fetch_add(1, std::memory_order_relaxed));
    }

    template <typename T>
    T& userData();

    void clear();

private:

    friend class CommandBufferAccessScope<B, true>;

    void end()
    {
        assert(!myPendingCommands.empty());

        if (myPendingCommands.back().end())
            enqueueOnePending();
    }
    
    void enqueueOnePending();
    void enqueueAllPendingToSubmitted(uint64_t timelineValue);

    CommandContextCreateDesc<B> myDesc = {};
    std::list<CommandBufferArray<B>> myPendingCommands;
    std::list<CommandBufferArray<B>> mySubmittedCommands;
    std::list<CommandBufferArray<B>> myFreeCommands;
    std::list<std::pair<std::function<void(uint64_t)>, uint64_t>> myGarbageCollectCallbacks;
    std::optional<uint64_t> myLastSubmitTimelineValue;
    std::vector<std::byte> myScratchMemory;
    std::any myUserData;
};

#include "command-vulkan.h"
