#pragma once

#include "device.h"
#include "types.h"

#include <array>
#include <functional>
#include <list>
#include <memory>
#include <optional>
#include <utility>

template <GraphicsApi G>
class Queue;

template <GraphicsApi G>
struct CommandBufferArrayCreateDesc
{
	CommandPoolHandle<G> pool{};
	uint8_t level = 0; // 0: primary, >= 1: secondary
	bool useBufferReset = false;
};

template <GraphicsApi G>
class CommandBufferArray final : public DeviceObject<G>
{
	static constexpr uint32_t kHeadBitCount = 2;
	static constexpr size_t kCommandBufferCount = (1 << kHeadBitCount);

public:
	constexpr CommandBufferArray() noexcept = default;
	CommandBufferArray(
		const std::shared_ptr<Device<G>>& device,
		CommandBufferArrayCreateDesc<G>&& desc);
	CommandBufferArray(CommandBufferArray&& other) noexcept;
	~CommandBufferArray() override;

	[[nodiscard]] CommandBufferArray& operator=(CommandBufferArray&& other) noexcept;
	[[nodiscard]] CommandBufferHandle<G> operator[](uint8_t index) const { return myArray[index]; }

	void Swap(CommandBufferArray& rhs) noexcept;
	friend void Swap(CommandBufferArray& lhs, CommandBufferArray& rhs) noexcept { lhs.Swap(rhs); }

	[[nodiscard]] const auto& GetDesc() const noexcept { return myDesc; }

	[[nodiscard]] static constexpr auto Capacity() { return kCommandBufferCount; }

	[[nodiscard]] uint8_t Begin(const CommandBufferBeginInfo<G>& beginInfo);
	void End(uint8_t index);

	void Reset();

	[[nodiscard]] uint8_t Head() const { return myBits.head; }
	[[nodiscard]] const CommandBufferHandle<G>* Data() const
	{
		ASSERT(!RecordingFlags());
		return myArray.data();
	}

	[[nodiscard]] bool Recording(uint8_t index) const
	{
		return myBits.recordingFlags & (1 << index);
	}
	[[nodiscard]] uint8_t RecordingFlags() const noexcept { return myBits.recordingFlags; }

	[[nodiscard]] bool Full() const { return (Head() + 1) >= Capacity(); }

private:
	CommandBufferArray(
		const std::shared_ptr<Device<G>>& device,
		std::tuple<
			CommandBufferArrayCreateDesc<G>,
			std::array<CommandBufferHandle<G>, kCommandBufferCount>>&& descAndData);

	CommandBufferArrayCreateDesc<G> myDesc{};
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

	[[nodiscard]] CommandBufferAccessScopeDesc<G>& operator=(const CommandBufferAccessScopeDesc<G>& other) noexcept;
	[[nodiscard]] bool operator==(const CommandBufferAccessScopeDesc<G>& other) const noexcept;

	CommandBufferInheritanceInfo<kVk> inheritance{};
	uint8_t level = 0; // 0: primary, >= 1: secondary
	bool scopedBeginEnd = true;
};

template <GraphicsApi G>
class CommandBufferAccessScope final
{
public:
	constexpr CommandBufferAccessScope() noexcept = default;
	CommandBufferAccessScope(
		CommandBufferArray<G>* array, const CommandBufferAccessScopeDesc<G>& beginInfo);
	CommandBufferAccessScope(const CommandBufferAccessScope& other);
	CommandBufferAccessScope(CommandBufferAccessScope&& other) noexcept;
	~CommandBufferAccessScope();

	[[nodiscard]] CommandBufferAccessScope<G>& operator=(CommandBufferAccessScope<G> other);
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
struct CommandPoolCreateDesc
{
	CommandPoolCreateFlags<G> flags{};
	uint32_t queueFamilyIndex = 0UL;
	uint32_t levelCount = 1;
};

template <GraphicsApi G>
class CommandPool : public DeviceObject<G>
{
public:
	constexpr CommandPool() noexcept = default;
	CommandPool(
		const std::shared_ptr<Device<G>>& device,
		CommandPoolCreateDesc<G>&& desc);
	CommandPool(CommandPool&& other) noexcept;
	~CommandPool() override;

	CommandPool& operator=(CommandPool&& other) noexcept;
	[[nodiscard]] operator auto() const noexcept { return myPool; }//NOLINT(google-explicit-constructor)

	[[nodiscard]] const auto& GetDesc() const noexcept { return myDesc; }

	void Swap(CommandPool& rhs) noexcept;
	friend void Swap(CommandPool& lhs, CommandPool& rhs) noexcept { lhs.Swap(rhs); }

	void Reset();

	[[nodiscard]] CommandBufferAccessScope<G> Commands(const CommandBufferAccessScopeDesc<G>& beginInfo = {});

private:
	friend class Queue<G>;

	CommandPool(
		const std::shared_ptr<Device<G>>& device,
		std::tuple<CommandPoolCreateDesc<G>, CommandPoolHandle<G>>&& descAndData);

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

	CommandPoolCreateDesc<G> myDesc{};
	CommandPoolHandle<G> myPool{};
	std::vector<CommandBufferListType<G>> myPendingCommands;
	std::vector<CommandBufferListType<G>> mySubmittedCommands;
	std::vector<CommandBufferListType<G>> myFreeCommands;
	std::vector<std::optional<CommandBufferAccessScope<G>>> myRecordingCommands;
};

#include "command.inl"
