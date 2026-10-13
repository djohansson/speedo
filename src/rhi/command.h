#pragma once

#include <rhi/deviceobject.h>
#include <rhi/rendertarget.h>
#include <rhi/types.h>

#include <cstdint>
#include <vector>

namespace rhi
{

template <GraphicsApi G>
class CommandPool;

// a command buffer being recorded, with a single owner. what records into it borrows its handle (it converts to a
// CommandBufferHandle); only its owner can end it, and submit it (Queue::EnqueueSubmit) or execute it
// (CommandEncoder::ExecuteCommands, a secondary one), which take it. made by its pool (CommandPool::Begin,
// BeginSecondary) and used under the lock of the queue owning the pool, and not kept past the pool's next reset
// (Queue::SwapAndResetPool checks)
template <GraphicsApi G>
class CommandBuffer final
{
public:
	constexpr CommandBuffer() noexcept = default;
	// a begun command buffer of pool's: see CommandPool::Begin
	CommandBuffer(CommandPool<G>& pool, CommandBufferHandle<G> handle, bool secondary) noexcept;
	CommandBuffer(CommandBuffer&& other) noexcept;
	CommandBuffer(const CommandBuffer&) = delete;
	// dropped without being submitted (an error path), it is ended and handed back to its pool unrun, which validation
	// builds warn about
	~CommandBuffer();

	[[maybe_unused]] CommandBuffer& operator=(CommandBuffer&& other) noexcept;
	CommandBuffer& operator=(const CommandBuffer&) = delete;

	[[nodiscard]] operator CommandBufferHandle<G>() const noexcept { return myHandle; } //NOLINT(google-explicit-constructor)

	[[nodiscard]] bool IsValid() const noexcept { return myHandle != nullptr; }
	[[nodiscard]] bool IsRecording() const noexcept { return myRecording; }
	[[nodiscard]] bool IsSecondary() const noexcept { return mySecondary; }

	// ends recording (Release does it too, if it is still recording)
	void End();

	// ends it if it is still recording, hands it back to its pool (which reuses it once it is reset, i.e. once the gpu is
	// done with it) and returns its handle, to be submitted or executed right away. it is empty afterwards
	[[nodiscard]] CommandBufferHandle<G> Release();

private:
	void InternalSwap(CommandBuffer& other) noexcept;

	CommandPool<G>* myPool = nullptr;
	CommandBufferHandle<G> myHandle{};
	bool mySecondary = false;
	bool myRecording = false;
};

template <GraphicsApi G>
struct CommandPoolCreateDesc final : DeviceObjectCreateDesc<G>
{
	CommandPoolCreateFlags<G> flags{};
	uint32_t queueFamilyIndex = 0;
};

template <GraphicsApi G>
struct ObjectTraits<CommandPool<G>>
{
	using CreateDescType = CommandPoolCreateDesc<G>;
};

// a queue's command buffers (see Queue::GetPool): handed out one at a time (CommandBuffer), taken back once submitted,
// and made reusable all at once by Reset (when the gpu is done with everything recorded from it). allocated as needed,
// several at a time, and kept until the pool goes
template <GraphicsApi G>
class CommandPool final : public DeviceObject<CommandPool<G>>
{
public:
	using SuperType = DeviceObject<CommandPool<G>>;
	using CreateDescType = ObjectTraits<CommandPool<G>>::CreateDescType;

	constexpr CommandPool() noexcept = default;
	explicit CommandPool(CreateDescType&& desc);
	CommandPool(CommandPool&& other) noexcept;
	~CommandPool();

	[[maybe_unused]] CommandPool& operator=(CommandPool&& other) noexcept;
	[[nodiscard]] operator auto() const noexcept { return myPool; }//NOLINT(google-explicit-constructor)

	void Swap(CommandPool& rhs) noexcept;
	friend void Swap(CommandPool& lhs, CommandPool& rhs) noexcept { lhs.Swap(rhs); }

	// makes every command buffer handed out reusable: with none still held (each submitted or dropped) and the gpu done
	// with them
	void Reset();

	// a primary command buffer, begun (one time submit)
	[[nodiscard]] CommandBuffer<G> Begin();
	// a secondary command buffer, begun for recording inside the render target begun with renderTarget (see
	// IRenderTarget::Begin with SubpassContents::kSecondaryCommandBuffers). its commands run where
	// CommandEncoder::ExecuteCommands puts them
	[[nodiscard]] CommandBuffer<G> BeginSecondary(const RenderTargetBeginInfo<G>& renderTarget);
	// a primary command buffer's handle, not begun, for one use by the caller before the pool's next reset (e.g. a
	// profiler's setup, which records and submits it itself)
	[[nodiscard]] CommandBufferHandle<G> Allocate();

	// takes back a command buffer handed out (see CommandBuffer::Release), to reuse once the pool is reset
	void Retire(CommandBufferHandle<G> handle, bool secondary);

	// how many command buffers are handed out and not yet taken back
	[[nodiscard]] uint32_t GetOutstandingCount() const noexcept { return myOutstanding; }

private:
	CommandPool(CreateDescType&& desc, CommandPoolHandle<G>&& pool);

	[[nodiscard]] CommandBufferHandle<G> InternalTake(bool secondary);

	CommandPoolHandle<G> myPool{};
	// per kind (primary, secondary): ready to begin, and handed out since the last reset
	std::array<std::vector<CommandBufferHandle<G>>, 2> myFree;
	std::array<std::vector<CommandBufferHandle<G>>, 2> myUsed;
	uint32_t myOutstanding = 0;
};

} // namespace rhi

#include "command.inl"
