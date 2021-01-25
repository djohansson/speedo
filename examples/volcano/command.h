#pragma once

#include "device.h"
#include "queue.h"
#include "types.h"

#include <array>
#include <functional>
#include <list>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <utility>
#include <vector>

template <GraphicsBackend B>
struct CommandBufferArrayCreateDesc : public DeviceResourceCreateDesc<B>
{
    CommandPoolHandle<B> pool = 0;
    CommandBufferLevel<B> level = {};
};

template <GraphicsBackend B>
class CommandBufferArray : public DeviceResource<B>
{
    static constexpr uint32_t kHeadBitCount = 2;
    static constexpr size_t kCommandBufferCount = (1 << kHeadBitCount);

public:

    CommandBufferArray() = default;
    CommandBufferArray(
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        CommandBufferArrayCreateDesc<B>&& desc);
    CommandBufferArray(CommandBufferArray&& other) noexcept;
    ~CommandBufferArray();

    CommandBufferArray& operator=(CommandBufferArray&& other) noexcept;

    void swap(CommandBufferArray& rhs) noexcept;
    friend void swap(CommandBufferArray& lhs, CommandBufferArray& rhs) noexcept { lhs.swap(rhs); }

    const auto& getDesc() const { return myDesc; }
    static constexpr auto capacity() { return kCommandBufferCount; }

    uint8_t begin(const CommandBufferBeginInfo<B>& beginInfo);
    void end(uint8_t index);

    void resetAll();

    uint8_t head() const { return myBits.head; }
    const CommandBufferHandle<B>* data() const { assert(!recordingFlags()); return myArray.data(); }
    
    bool recording(uint8_t index) const { return myBits.recordingFlags & (1 << index); }
    uint8_t recordingFlags() const { return myBits.recordingFlags; }
    
    bool full() const { return (head() + 1) >= capacity(); }
    
    CommandBufferHandle<B>& operator[](uint8_t index) { return myArray[index]; }
    const CommandBufferHandle<B>& operator[](uint8_t index) const { return myArray[index]; }

private:

    CommandBufferArray(
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        std::tuple<CommandBufferArrayCreateDesc<B>, std::array<CommandBufferHandle<B>, kCommandBufferCount>>&& descAndData);
        
    CommandBufferArrayCreateDesc<B> myDesc = {};
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
struct CommandBufferAccessScopeDesc : CommandBufferBeginInfo<B>
{
    CommandBufferAccessScopeDesc();
    CommandBufferAccessScopeDesc(const CommandBufferAccessScopeDesc<B>& other);

    CommandBufferAccessScopeDesc<B>& operator=(const CommandBufferAccessScopeDesc<B>& other);
    bool operator==(const CommandBufferAccessScopeDesc<B>& other) const;

    CommandBufferLevel<B> level = {};
    CommandBufferInheritanceInfo<Vk> inheritance = {};
};

template <GraphicsBackend B>
class CommandBufferAccessScope : Nondynamic
{
public:

    CommandBufferAccessScope() = default;
    CommandBufferAccessScope(
        CommandBufferArray<B>* array,
        const CommandBufferAccessScopeDesc<B>& beginInfo);
    CommandBufferAccessScope(const CommandBufferAccessScope& other);
    CommandBufferAccessScope(CommandBufferAccessScope&& other) noexcept;
    ~CommandBufferAccessScope();

    CommandBufferAccessScope<B>& operator=(CommandBufferAccessScope<B> other);
    operator auto() const { return (*myArray)[myIndex]; }

    void swap(CommandBufferAccessScope<B>& rhs) noexcept;
    friend void swap(CommandBufferAccessScope<B>& lhs, CommandBufferAccessScope<B>& rhs) noexcept { lhs.swap(rhs); }

    const auto& getDesc() const { return myDesc; }

    void end() { myArray->end(myIndex); }

private:

    CommandBufferAccessScopeDesc<B> myDesc = {};
    std::shared_ptr<uint32_t> myRefCount;
    CommandBufferArray<B>* myArray = nullptr;
    uint8_t myIndex = 0;
};

template <GraphicsBackend B>
class CommandContext
{
    using CommandBufferListType = std::list<
        std::pair<
            CommandBufferArray<B>,
            std::pair<
                uint64_t,
                std::reference_wrapper<CommandContext<B>>>>>;

public:

    CommandContext(
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        CommandContextCreateDesc<B>&& desc);
    ~CommandContext();

    const auto& getDesc() const { return myDesc; }

    CommandBufferAccessScope<B> commands(const CommandBufferAccessScopeDesc<B>& beginInfo = {});
    
    uint64_t execute(CommandContext<B>& callee);
    QueueSubmitInfo<B> flush(QueueSyncInfo<B>&& syncInfo);

    // these will be complete when the timeline value is reached of the command buffer they are submitted in.
    // useful for ensuring that dependencies are respected when releasing resources. do not remove.
    void addCommandsFinishedCallback(std::function<void(uint64_t)>&& callback);

protected:

    const auto& getDeviceContext() const { return myDevice; }

private:    

    CommandBufferAccessScope<B> internalBeginScope(const CommandBufferAccessScopeDesc<B>& beginInfo);
    CommandBufferAccessScope<B> internalCommands(const CommandBufferAccessScopeDesc<B>& beginInfo) const;
    void internalEndCommands(CommandBufferLevel<B> level);
    
    void enqueueOnePending(CommandBufferLevel<B> level);
    void enqueueExecuted(CommandBufferListType&& commands, uint64_t timelineValue);
    void enqueueSubmitted(CommandBufferListType&& commands, uint64_t timelineValue);

    std::shared_ptr<DeviceContext<B>> myDevice;
    const CommandContextCreateDesc<B> myDesc = {};
    std::vector<CommandBufferListType> myPendingCommands;
    CommandBufferListType myExecutedCommands;
    CommandBufferListType mySubmittedCommands;
    std::vector<CommandBufferListType> myFreeCommands;
    std::vector<std::optional<CommandBufferAccessScope<B>>> myRecordingCommands;
    std::list<std::function<void(uint64_t)>> mySubmitFinishedCallbacks;
};

#include "command.inl"
