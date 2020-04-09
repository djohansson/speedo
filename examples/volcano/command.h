#pragma once

#include "gfx-types.h"

template <GraphicsBackend B>
struct CommandCreateDesc
{
    DeviceHandle<B> device = 0;
    QueueHandle<B> queue = 0;
    CommandPoolHandle<B> commandPool = 0;
    SemaphoreHandle<B> timelineSemaphore = 0;
    uint64_t commandsBeginWaitValue = 0;
    uint64_t commandsEndSignalValue = 1;
};

template <GraphicsBackend B>
class CommandContext : Noncopyable
{
public:

    CommandContext(CommandContext<B>&& context) = default;
    CommandContext(CommandCreateDesc<B>&& desc);
    ~CommandContext();
    
    const auto getCommandBuffer() const { return myCommandBuffer; }

    void begin() const;
    void submit() const;
    void end() const;
    bool isComplete() const;
    void sync() const;

private:

    const CommandCreateDesc<B> myDesc = {};
    CommandBufferHandle<B> myCommandBuffer = 0;
};
