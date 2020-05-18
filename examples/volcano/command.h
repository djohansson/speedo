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
#include <mutex>
#include <utility>
#include <vector>

template <GraphicsBackend B>
class CommandContext;

template <GraphicsBackend B, bool EndOnDestruct>
class CommandBufferAccessScope;

template <GraphicsBackend B>
struct CommandBufferArrayCreateDesc : public DeviceResourceCreateDesc<B>
{
    CommandPoolHandle<B> commandPool = 0;
    uint32_t commandBufferLevel = 0;
};

template <GraphicsBackend B>
class CommandBufferArray : DeviceResource<B>
{
    friend class CommandContext<B>;
    friend class CommandBufferAccessScope<B, true>;
    friend class CommandBufferAccessScope<B, false>;

    static constexpr uint32_t kCommandBufferCount = 8;

public:

    CommandBufferArray(CommandBufferArray<B>&& other) = default;
    CommandBufferArray(
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        std::tuple<CommandBufferArrayCreateDesc<B>, std::array<CommandBufferHandle<B>, kCommandBufferCount>>&& descAndData);
    CommandBufferArray(
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        CommandBufferArrayCreateDesc<B>&& desc);
    ~CommandBufferArray();

private:

    void begin(const CommandBufferBeginInfo<B>* beginInfo = nullptr);
    bool end();
    void reset();

    const CommandBufferHandle<B>* data() const { assert(!recording()); return myCommandBufferArray.data(); }
    uint8_t size() const { static_assert(kCommandBufferCount < 128); assert(!recording()); return myBits.myHead; }
    bool recording() const { return myBits.myRecording; }
    bool full() const { return (size() + 1) >= capacity(); }
    constexpr auto capacity() const { return kCommandBufferCount; }
    
    operator CommandBufferHandle<B>() const { return myCommandBufferArray[myBits.myHead]; }

    static auto createArray(
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        const CommandBufferArrayCreateDesc<B>& desc);
        
    const CommandBufferArrayCreateDesc<B> myDesc = {};
    std::array<CommandBufferHandle<B>, kCommandBufferCount> myCommandBufferArray = {};
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

    void end()
    {
        myArray.end();
    }

    ~CommandBufferAccessScope()
    {
        if constexpr (CallBeginAndEnd)
            if (myArray.recording())
                myArray.end();
            
    }

    operator CommandBufferHandle<B>() const { return myArray; }

private:

    CommandBufferArray<B>& myArray;
};

template <GraphicsBackend B>
struct CommandContextCreateDesc
{
    CommandPoolHandle<B> commandPool = 0;
    uint32_t commandBufferLevel = 0;
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
class CommandContext
{
    friend class CommandBufferArray<B>;
    friend class CommandBufferAccessScope<B, true>;

public:

    CommandContext(CommandContext<B>&& other) = default;
    CommandContext(
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        CommandContextCreateDesc<B>&& desc);
    ~CommandContext();

    const auto& getDesc() const { return myDesc; }

    auto beginScope(const CommandBufferBeginInfo<B>* beginInfo = nullptr) 
    {
        std::lock_guard<decltype(myCommandsMutex)> guard(myCommandsMutex);
        return internalBeginScope(beginInfo);
    }
    auto commands()
    {
        std::lock_guard<decltype(myCommandsMutex)> guard(myCommandsMutex);
        return internalCommands();
    }
    
    uint64_t execute(CommandContext<B>& other);
    uint64_t submit(const CommandSubmitInfo<B>& submitInfo = CommandSubmitInfo<B>());
    void reset();

protected:

    const auto& getDeviceContext() const { return myDeviceContext; }

private:

    CommandBufferAccessScope<GraphicsBackend::Vulkan, true> internalBeginScope(const CommandBufferBeginInfo<B>* beginInfo);
    CommandBufferAccessScope<GraphicsBackend::Vulkan, false> internalCommands();
    
    void enqueueOnePending();
    void enqueueAllPendingToSubmitted(uint64_t timelineValue);

    std::shared_ptr<DeviceContext<B>> myDeviceContext;
    const CommandContextCreateDesc<B> myDesc = {};
    std::list<std::pair<CommandBufferArray<B>, uint64_t>> myPendingCommands;
    std::list<std::pair<CommandBufferArray<B>, uint64_t>> mySubmittedCommands;
    std::list<std::pair<CommandBufferArray<B>, uint64_t>> myFreeCommands;
    std::shared_mutex myCommandsMutex;
    std::vector<std::byte> myScratchMemory;
};
