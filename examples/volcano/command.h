#pragma once

#include "device.h"
#include "gfx-types.h"

#include <atomic>
#include <cassert>
#include <functional>
#include <list>
#include <memory>
#include <utility>
//#include <vector>

template <GraphicsBackend B>
class CommandContext;

template <GraphicsBackend B>
class CommandBufferAccessScope;

template <GraphicsBackend B>
struct CommandBufferArrayDesc
{
    std::shared_ptr<DeviceContext<B>> deviceContext;
    CommandPoolHandle<B> commandPool = 0;
    uint32_t commandBufferLevel = 0;
};

template <GraphicsBackend B>
class CommandBufferArray : Noncopyable
{
public:

    CommandBufferArray(CommandBufferArray<B>&& other)
    : myArrayDesc(other.myArrayDesc)
    , myIndex(other.myIndex)
    {
        other.myArrayDesc = {};
        std::copy(std::begin(other.myCommandBufferArray), std::end(other.myCommandBufferArray), std::begin(myCommandBufferArray));
        std::fill(std::begin(other.myCommandBufferArray), std::end(other.myCommandBufferArray), static_cast<CommandBufferHandle<B>>(0));
        other.myIndex = 0;
    }
    CommandBufferArray(CommandBufferArrayDesc<B>&& desc);
    ~CommandBufferArray();

private:

    friend class CommandContext<B>;
    friend class CommandBufferAccessScope<B>;

    void begin(const CommandBufferBeginInfo<B>* beginInfo = nullptr) const;
    bool end();

    const CommandBufferHandle<B>* data() const { return myCommandBufferArray; }
    uint64_t& index() { return myIndex; }
    const uint64_t& index() const { return myIndex; }
    uint64_t& timelineValue() { return myTimelineValue; }
    const uint64_t& timelineValue() const { return myTimelineValue; }

    operator CommandBufferHandle<B>() const { return myCommandBufferArray[myIndex]; }

    static constexpr uint32_t kCommandBufferCount = 8;

    CommandBufferArrayDesc<B> myArrayDesc = {};
    CommandBufferHandle<B> myCommandBufferArray[kCommandBufferCount] = { static_cast<CommandBufferHandle<B>>(0) };
    union 
    {
        uint64_t myIndex = 0;
        uint64_t myTimelineValue;
    };
};

template <GraphicsBackend B>
class CommandBufferAccessScope : Noncopyable, Nondynamic
{
public:

    CommandBufferAccessScope(CommandBufferArray<B>& array)
        : myArray(array)
    { }

    operator CommandBufferHandle<B>() const { return myArray; }

private:

    CommandBufferArray<B>& myArray;
};

template <GraphicsBackend B>
struct CommandContextDesc
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
    uint32_t waitSemaphoreCount = 0;
    const SemaphoreHandle<B>* waitSemaphores = nullptr;
    const Flags<B>* waitDstStageMasks = 0;
    uint32_t signalSemaphoreCount = 0;
    const SemaphoreHandle<B>* signalSemaphores = nullptr;
    FenceHandle<B> signalFence = 0;
};

template <GraphicsBackend B>
class CommandContext : private Noncopyable
{
public:

    CommandContext(CommandContext<B>&& other)
    : myCommandContextDesc(other.myCommandContextDesc)
    , myPendingCommands(std::move(other.myPendingCommands))
    , mySyncCallbacks(std::move(other.mySyncCallbacks))
    , myLastSubmitTimelineValue(std::move(other.myLastSubmitTimelineValue))
    {
        other.myCommandContextDesc = {};
    }
    CommandContext(CommandContextDesc<B>&& desc);
    ~CommandContext();

    const auto& getCommandContextDesc() const { return myCommandContextDesc; }
    const auto& getLastSubmitTimelineValue() const { return myLastSubmitTimelineValue; }

    auto begin(const CommandBufferBeginInfo<B>* beginInfo = nullptr)
    {
        if (myPendingCommands.empty())
            enqueueOnePending();

        myPendingCommands.back().begin(beginInfo);

        return CommandBufferAccessScope<B>(myPendingCommands.back());
    }
    auto commands() { return CommandBufferAccessScope<B>(myPendingCommands.back()); }
    void end()
    {
        assert(!myPendingCommands.empty());

        if (myPendingCommands.back().end())
            enqueueOnePending();
    }

    uint64_t execute(CommandContext<B>& other, const RenderPassBeginInfo<B>* beginInfo);
    uint64_t submit(const CommandSubmitInfo<B>& submitInfo = CommandSubmitInfo<B>());

    bool hasReached(uint64_t timelineValue) const;
    void sync(std::optional<uint64_t> timelineValue = std::nullopt);

    template <typename T>
    uint64_t addSyncCallback(T callback, uint64_t timelineValue)
    {
        mySyncCallbacks.emplace_back(std::make_pair(callback, timelineValue));
        return timelineValue;
    }

    template <typename T>
    uint64_t addSyncCallback(T callback)
    {
        return addSyncCallback(callback, myCommandContextDesc.timelineValue->fetch_add(1, std::memory_order_relaxed));
    }

private:

    friend class CommandBufferArray<B>;

    void enqueueOnePending();
    void enqueueAllPendingToSubmitted(uint64_t timelineValue);

    CommandContextDesc<B> myCommandContextDesc = {};
    std::list<CommandBufferArray<B>> myPendingCommands;
    std::list<CommandBufferArray<B>> mySubmittedCommands;
    std::list<CommandBufferArray<B>> myFreeCommands;
    std::list<std::pair<std::function<void()>, uint64_t>> mySyncCallbacks;
    std::optional<uint64_t> myLastSubmitTimelineValue;

    static thread_local std::vector<std::byte> threadScratchMemory;
};
