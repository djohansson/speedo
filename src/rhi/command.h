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
	~CommandBufferArray();

	CommandBufferArray& operator=(CommandBufferArray&& other) noexcept;
	CommandBufferHandle<G> operator[](uint8_t index) const { return myArray[index]; }

	void Swap(CommandBufferArray& rhs) noexcept;
	friend void Swap(CommandBufferArray& lhs, CommandBufferArray& rhs) noexcept { lhs.Swap(rhs); }

	const auto& GetDesc() const noexcept { return myDesc; }

	static constexpr auto Capacity() { return kCommandBufferCount; }

	uint8_t Begin(const CommandBufferBeginInfo<G>& beginInfo);
	void End(uint8_t index);

	void Reset();

	uint8_t Head() const { return myBits.head; }
	const CommandBufferHandle<G>* Data() const
	{
		ASSERT(!RecordingFlags());
		return myArray.data();
	}

	bool Recording(uint8_t index) const { return myBits.recordingFlags & (1 << index); }
	uint8_t RecordingFlags() const noexcept { return myBits.recordingFlags; }

	bool Full() const { return (Head() + 1) >= Capacity(); }

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
	CommandBufferAccessScopeDesc(bool scopedBeginEnd = true);
	CommandBufferAccessScopeDesc(const CommandBufferAccessScopeDesc<G>& other);

	CommandBufferAccessScopeDesc<G>& operator=(const CommandBufferAccessScopeDesc<G>& other);
	bool operator==(const CommandBufferAccessScopeDesc<G>& other) const;

	CommandBufferInheritanceInfo<Vk> inheritance{};
	uint8_t level = 0; // 0: primary, >= 1: secondary
	bool scopedBeginEnd = true;
};

template <GraphicsApi G>
class CommandBufferAccessScope final : public Nondynamic
{
public:
	constexpr CommandBufferAccessScope() noexcept = default;
	CommandBufferAccessScope(
		CommandBufferArray<G>* array, const CommandBufferAccessScopeDesc<G>& beginInfo);
	CommandBufferAccessScope(const CommandBufferAccessScope& other);
	CommandBufferAccessScope(CommandBufferAccessScope&& other) noexcept;
	~CommandBufferAccessScope();

	CommandBufferAccessScope<G>& operator=(CommandBufferAccessScope<G> other);
	operator auto() const { return (*myArray)[myIndex]; }

	void Swap(CommandBufferAccessScope<G>& rhs) noexcept;
	friend void Swap(CommandBufferAccessScope<G>& lhs, CommandBufferAccessScope<G>& rhs) noexcept
	{
		lhs.Swap(rhs);
	}

	const auto& GetDesc() const noexcept { return myDesc; }

	void Begin() { myIndex = myArray->Begin(myDesc); }
	void End() const { myArray->End(myIndex); }

private:
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
	uint32_t queueFamilyIndex = 0ul;
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
	~CommandPool();

	CommandPool& operator=(CommandPool&& other) noexcept;
	operator auto() const noexcept { return myPool; }

	const auto& GetDesc() const noexcept { return myDesc; }

	void Swap(CommandPool& rhs) noexcept;
	friend void Swap(CommandPool& lhs, CommandPool& rhs) noexcept { lhs.Swap(rhs); }

	void Reset();

	CommandBufferAccessScope<G> Commands(const CommandBufferAccessScopeDesc<G>& beginInfo = {});

private:
	friend class Queue<G>;

	CommandPool(
		const std::shared_ptr<Device<G>>& device,
		std::tuple<CommandPoolCreateDesc<G>, CommandPoolHandle<G>>&& descAndData);

	CommandBufferAccessScope<G> InternalBeginScope(const CommandBufferAccessScopeDesc<G>& beginInfo);
	void InternalEndCommands(uint8_t level);
	void InternalEnqueueOnePending(uint8_t level);
	void InternalEnqueueSubmitted(CommandBufferListType<G>&& cbList, uint8_t level, uint64_t timelineValue);

	auto& InternalGetPendingCommands() noexcept { return myPendingCommands; }
	const auto& InternalGetPendingCommands() const noexcept { return myPendingCommands; }

	auto& InternalGetSubmittedCommands() noexcept { return mySubmittedCommands; }
	const auto& InternalGetSubmittedCommands() const noexcept { return mySubmittedCommands; }

	CommandPoolCreateDesc<G> myDesc{};
	CommandPoolHandle<G> myPool{};
	std::vector<CommandBufferListType<G>> myPendingCommands;
	std::vector<CommandBufferListType<G>> mySubmittedCommands;
	std::vector<CommandBufferListType<G>> myFreeCommands;
	std::vector<std::optional<CommandBufferAccessScope<G>>> myRecordingCommands;
};

#include "command.inl"
