#pragma once

#include "device.h"
#include "deviceresource.h"
#include "gfx-types.h"

#include <atomic>
#include <array>
#include <cassert>
#include <functional>
#include <list>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <utility>
#include <vector>

template <GraphicsBackend B>
class CommandContext;

template <GraphicsBackend B>
struct CommandBufferArrayCreateDesc : public DeviceResourceCreateDesc<B>
{
    CommandPoolHandle<B> pool = 0;
    CommandBufferLevel<B> level = {};
};

template <GraphicsBackend B>
class CommandBufferArray : DeviceResource<B>
{
public:

    static constexpr uint32_t kHeadBitCount = 2;
    static constexpr uint32_t kCommandBufferCount = (1 << kHeadBitCount);

    CommandBufferArray(CommandBufferArray<B>&& other) = default;
    CommandBufferArray(
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        std::tuple<CommandBufferArrayCreateDesc<B>, std::array<CommandBufferHandle<B>, kCommandBufferCount>>&& descAndData);
    CommandBufferArray(
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        CommandBufferArrayCreateDesc<B>&& desc);
    ~CommandBufferArray();

    const auto& getDesc() const { return myDesc; }

    uint8_t begin(const CommandBufferBeginInfo<B>& beginInfo);
    void end(uint8_t index);

    void resetAll();

    uint8_t head() const { return myBits.head; }
    const CommandBufferHandle<B>* data() const { assert(!recordingFlags()); return myArray.data(); }
    
    bool recording(uint8_t index) const { return myBits.recordingFlags & (1 << index); }
    uint8_t recordingFlags() const { return myBits.recordingFlags; }
    
    bool full() const { return (head() + 1) >= capacity(); }
    constexpr auto capacity() const { return kCommandBufferCount; }
    
    CommandBufferHandle<B>& operator[](uint8_t index) { return myArray[index]; }
    const CommandBufferHandle<B>& operator[](uint8_t index) const { return myArray[index]; }

private:
        
    const CommandBufferArrayCreateDesc<B> myDesc = {};
    std::array<CommandBufferHandle<B>, kCommandBufferCount> myArray = {};
    struct Bits
    {
        uint8_t head : kHeadBitCount;
        uint8_t recordingFlags : kCommandBufferCount;
    } myBits = {0, 0};
};

template <GraphicsBackend B>
struct CommandContextCreateDesc
{
    CommandPoolHandle<B> pool = 0;
};

template <GraphicsBackend B>
struct CommandSubmitInfo
{
    QueueHandle<B> queue = 0;
    uint32_t waitSemaphoreCount = 0;
    const SemaphoreHandle<B>* waitSemaphores = nullptr;
    const Flags<B>* waitDstStageMasks = nullptr;
    const uint64_t* waitSemaphoreValues = nullptr;
    uint32_t signalSemaphoreCount = 0;
    const SemaphoreHandle<B>* signalSemaphores = nullptr;
    const uint64_t* signalSemaphoreValues = nullptr;
    FenceHandle<B> signalFence = 0;
};

template <GraphicsBackend B>
struct CommandContextBeginInfo : public CommandBufferBeginInfo<B>
{
    CommandContextBeginInfo();

    bool operator==(const CommandContextBeginInfo& other) const;

    CommandBufferLevel<B> level = {};
};

template <GraphicsBackend B>
class CommandContext
{
public:

    CommandContext(CommandContext<B>&& other) = default;
    CommandContext(
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        CommandContextCreateDesc<B>&& desc);
    ~CommandContext();

    const auto& getDesc() const { return myDesc; }

    auto commands(CommandContextBeginInfo<B>&& beginInfo = {})
    {
        if (myLastCommands && (*myLastCommands).getBeginInfo() == beginInfo)
        {
            std::shared_lock readLock(myCommandsMutex);
            return internalCommands();
        }
        else
        {
            std::unique_lock writeLock(myCommandsMutex);
            return internalBeginScope(std::move(beginInfo));
        }
    }
    void endCommands()
    {
        if (myLastCommands)
            myLastCommands = std::nullopt;
    }
    
    uint64_t execute(CommandContext<B>& callee);
    uint64_t submit(const CommandSubmitInfo<B>& submitInfo = {});

    // these will be complete when the timeline value is reached of the command buffer they are submitted in
    void addSubmitFinishedCallback(std::function<void(uint64_t)>&& callback);

protected:

    const auto& getDeviceContext() const { return myDevice; }

private:

    class CommandBufferAccessScope : Noncopyable, Nondynamic
    {
    public:

        CommandBufferAccessScope(CommandBufferAccessScope&& other)
        : myArray(other.myArray)
        , myIndex(other.myIndex)
        , myBeginInfo(std::move(other.myBeginInfo))
        {
            other.myIndex = std::nullopt;
        };
        CommandBufferAccessScope(CommandBufferAccessScope const&& other)
        : myArray(other.myArray)
        , myIndex(other.myIndex)
        , myBeginInfo(std::move(other.myBeginInfo))
        {
            other.myIndex = std::nullopt;
        };
        CommandBufferAccessScope(CommandBufferArray<B>& array, CommandContextBeginInfo<B>&& beginInfo)
        : myArray(array)
        , myIndex(std::make_optional(myArray.begin(beginInfo)))
        , myBeginInfo(std::move(beginInfo)) {}
        ~CommandBufferAccessScope()
        {
            if (myIndex && myArray.recording(*myIndex))
                myArray.end(*myIndex);
        }

        const auto& getBeginInfo() const { return myBeginInfo; }

        operator CommandBufferHandle<B>() const { return myArray[*myIndex]; }

    private:

        CommandBufferArray<B>& myArray;
        std::optional<uint8_t> myIndex;
        CommandContextBeginInfo<B> myBeginInfo = {};
    };

    CommandBufferHandle<B> internalBeginScope(CommandContextBeginInfo<B>&& beginInfo);
    CommandBufferHandle<B> internalCommands() const;

    using CommandBufferList = std::list<std::pair<CommandBufferArray<B>, std::pair<uint64_t, std::reference_wrapper<CommandContext<B>>>>>;
    
    void enqueueOnePending(CommandBufferLevel<B> level);
    void enqueueExecuted(CommandBufferList&& commands, uint64_t timelineValue);
    void enqueueSubmitted(CommandBufferList&& commands, uint64_t timelineValue);

    std::shared_ptr<DeviceContext<B>> myDevice;
    const CommandContextCreateDesc<B> myDesc = {};
    std::vector<CommandBufferList> myPendingCommands;
    CommandBufferList myExecutedCommands;
    CommandBufferList mySubmittedCommands;
    std::vector<CommandBufferList> myFreeCommands;
    std::shared_mutex myCommandsMutex;
    std::vector<std::byte> myScratchMemory;
    std::optional<CommandBufferAccessScope> myLastCommands;
    std::list<std::function<void(uint64_t)>> mySubmitFinishedCallbacks;
};
