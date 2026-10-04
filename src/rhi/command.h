#pragma once

#include <rhi/deviceobject.h>
#include <rhi/rendertarget.h>
#include <rhi/types.h>

#include <array>
#include <list>
#include <memory>
#include <optional>

namespace rhi
{

template <GraphicsApi G>
class Queue;

template <GraphicsApi G>
class CommandBufferArray;

template <GraphicsApi G>
class CommandPool;

template <GraphicsApi G>
struct CommandBufferArrayCreateDesc final : DeviceObjectCreateDesc<G>
{
	CommandPoolHandle<G> pool{};
	uint8_t level : 6; // 0: primary, >= 1: secondary
	uint8_t useResetCommandBuffers : 1;
	uint8_t useReleaseResourcesOnReset : 1;
};

template <GraphicsApi G>
struct ObjectTraits<CommandBufferArray<G>>
{
	using CreateDescType = CommandBufferArrayCreateDesc<G>;
};

template <GraphicsApi G>
class CommandBufferArray final : public DeviceObject<CommandBufferArray<G>>
{
	static constexpr uint32_t kHeadBitCount = 2;
	static constexpr size_t kCommandBufferCount = (1 << kHeadBitCount);

public:
	using SuperType = DeviceObject<CommandBufferArray<G>>;
	using CreateDescType = ObjectTraits<CommandBufferArray<G>>::CreateDescType;

	constexpr CommandBufferArray() noexcept = default;
	explicit CommandBufferArray(CreateDescType&& desc);
	CommandBufferArray(CommandBufferArray&& other) noexcept;
	~CommandBufferArray();

	[[maybe_unused]] CommandBufferArray& operator=(CommandBufferArray&& other) noexcept;
	[[nodiscard]] CommandBufferHandle<G> operator[](uint8_t index) const { return myArray[index]; }

	void Swap(CommandBufferArray& rhs) noexcept;
	friend void Swap(CommandBufferArray& lhs, CommandBufferArray& rhs) noexcept { lhs.Swap(rhs); }

	[[nodiscard]] static consteval auto Capacity() { return kCommandBufferCount; }

	[[nodiscard]] uint8_t Begin(const CommandBufferBeginInfo<G>& beginInfo);
	void End(uint8_t index);

	void Reset();

	[[nodiscard]] uint8_t Head() const noexcept { return myBits.head; }
	[[nodiscard]] const CommandBufferHandle<G>* Data() const noexcept
	{
		ENSURE(!RecordingFlags());
		return myArray.data();
	}

	[[nodiscard]] bool Recording(uint8_t index) const noexcept
	{
		return myBits.recordingFlags & (1 << index);
	}
	[[nodiscard]] uint8_t RecordingFlags() const noexcept { return myBits.recordingFlags; }

	[[nodiscard]] bool Full() const noexcept { return (Head() + 1) >= Capacity(); }

private:
	explicit CommandBufferArray(
		CreateDescType&& desc,
		std::array<CommandBufferHandle<G>, kCommandBufferCount>&& array);

	std::array<CommandBufferHandle<G>, kCommandBufferCount> myArray{};
	struct Bits
	{
		uint8_t head : kHeadBitCount;
		uint8_t recordingFlags : kCommandBufferCount;
	} myBits{0, 0};
};

template <GraphicsApi G>
struct CommandBufferAccessScopeDesc final : public CommandBufferBeginInfo<G>
{
	CommandBufferAccessScopeDesc(bool scopedBeginEnd = true) noexcept;//NOLINT(google-explicit-constructor)
	CommandBufferAccessScopeDesc(const CommandBufferAccessScopeDesc<G>& other) noexcept;

	[[maybe_unused]] CommandBufferAccessScopeDesc<G>& operator=(const CommandBufferAccessScopeDesc<G>& other) noexcept;
	[[nodiscard]] bool operator==(const CommandBufferAccessScopeDesc<G>& other) const noexcept;

	CommandBufferInheritanceInfo<kVk> inheritance{};
	uint8_t level : 6; // 0: primary, >= 1: secondary
	uint8_t scopedBeginEnd : 1;
};

template <GraphicsApi G>
class CommandBufferAccessScope final
{
public:
	using CreateDescType = CommandBufferAccessScopeDesc<G>;

	constexpr CommandBufferAccessScope() noexcept = default;
	CommandBufferAccessScope(
		const CreateDescType& beginInfo,
		CommandBufferArray<G>* array);
	CommandBufferAccessScope(const CommandBufferAccessScope& other) noexcept;
	CommandBufferAccessScope(CommandBufferAccessScope&& other) noexcept;
	~CommandBufferAccessScope();

	[[maybe_unused]] CommandBufferAccessScope<G>& operator=(CommandBufferAccessScope<G>&& other) noexcept;
	[[nodiscard]] operator auto() const { return (*myArray)[myIndex]; }//NOLINT(google-explicit-constructor)

	void Swap(CommandBufferAccessScope<G>& rhs) noexcept;
	friend void Swap(CommandBufferAccessScope<G>& lhs, CommandBufferAccessScope<G>& rhs) noexcept
	{
		lhs.Swap(rhs);
	}

	[[nodiscard]] const auto& GetDesc() const noexcept { return myDesc; }

	void Begin() { myIndex = myArray->Begin(myDesc); }
	void End() const { myArray->End(myIndex); }

private:
	void* operator new(size_t);
	void* operator new[](size_t);
	CommandBufferAccessScopeDesc<G> myDesc{};
	std::shared_ptr<uint32_t> myRefCount;
	CommandBufferArray<G>* myArray = nullptr;
	uint8_t myIndex = 0;
};

template <GraphicsApi G>
using CommandBufferListType = std::list<std::tuple<CommandBufferArray<G>, uint64_t>>;

template <GraphicsApi G>
struct CommandPoolCreateDesc final : DeviceObjectCreateDesc<G>
{
	CommandPoolCreateFlags<G> flags{};
	uint32_t queueFamilyIndex : 27;
	uint32_t levelCount : 4;
	uint32_t supportsProfiling : 1;
};

template <GraphicsApi G>
struct ObjectTraits<CommandPool<G>>
{
	using CreateDescType = CommandPoolCreateDesc<G>;
};

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

	void Reset();

	[[nodiscard]] CommandBufferAccessScope<G> Commands(const CommandBufferAccessScopeDesc<G>& beginInfo = {});
	// begins a secondary command buffer at level (1 and up) for recording inside the render target begun with
	// renderTarget (see IRenderTarget::Begin with SubpassContents::kSecondaryCommandBuffers). its commands are run by
	// Queue::Execute.
	[[nodiscard]] CommandBufferAccessScope<G> SecondaryCommands(uint8_t level, const RenderTargetBeginInfo<G>& renderTarget);

private:
	friend class Queue<G>;
	CommandPool(
		CreateDescType&& desc,
		CommandPoolHandle<G>&& pool);

	[[nodiscard]] CommandBufferAccessScope<G> InternalBeginScope(const CommandBufferAccessScopeDesc<G>& beginInfo);
	void InternalEndCommands(uint8_t level);
	void InternalEnqueueOnePending(uint8_t level);
	void InternalEnqueueSubmitted(CommandBufferListType<G>&& cbList, uint8_t level, uint64_t timelineValue);

	[[nodiscard]] auto& InternalGetPendingCommands() noexcept { return myPendingCommands; }
	[[nodiscard]] const auto& InternalGetPendingCommands() const noexcept
	{
		return myPendingCommands;
	}

	[[nodiscard]] auto& InternalGetSubmittedCommands() noexcept { return mySubmittedCommands; }
	[[nodiscard]] const auto& InternalGetSubmittedCommands() const noexcept
	{
		return mySubmittedCommands;
	}

	CommandPoolHandle<G> myPool{};
	std::vector<CommandBufferListType<G>> myPendingCommands;
	std::vector<CommandBufferListType<G>> mySubmittedCommands;
	std::vector<CommandBufferListType<G>> myFreeCommands;
	std::vector<std::optional<CommandBufferAccessScope<G>>> myRecordingCommands;
};

} // namespace rhi

#include "command.inl"
