#pragma once

#include "device.h"
#include "queue.h"
#include "types.h"

#include <array>
#include <functional>
#include <list>
#include <memory>
#include <optional>
#include <utility>

template <GraphicsApi G>
struct CommandBufferArrayCreateDesc
{
	CommandPoolHandle<G> pool{};
	CommandBufferLevel<G> level{};
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

	void swap(CommandBufferArray& rhs) noexcept;
	friend void swap(CommandBufferArray& lhs, CommandBufferArray& rhs) noexcept { lhs.swap(rhs); }

	const auto& getDesc() const noexcept { return myDesc; }

	static constexpr auto capacity() { return kCommandBufferCount; }

	uint8_t begin(const CommandBufferBeginInfo<G>& beginInfo);
	void end(uint8_t index);

	void reset();

	uint8_t head() const { return myBits.head; }
	const CommandBufferHandle<G>* data() const
	{
		assert(!recordingFlags());
		return myArray.data();
	}

	bool recording(uint8_t index) const { return myBits.recordingFlags & (1 << index); }
	uint8_t recordingFlags() const noexcept { return myBits.recordingFlags; }

	bool full() const { return (head() + 1) >= capacity(); }

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

	CommandBufferLevel<G> level{};
	CommandBufferInheritanceInfo<Vk> inheritance{};
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

	void swap(CommandBufferAccessScope<G>& rhs) noexcept;
	friend void swap(CommandBufferAccessScope<G>& lhs, CommandBufferAccessScope<G>& rhs) noexcept
	{
		lhs.swap(rhs);
	}

	const auto& getDesc() const { return myDesc; }

	void begin() { myIndex = myArray->begin(myDesc); }
	void end() const { myArray->end(myIndex); }

private:
	CommandBufferAccessScopeDesc<G> myDesc{};
	std::shared_ptr<uint32_t> myRefCount;
	CommandBufferArray<G>* myArray = nullptr;
	uint8_t myIndex = 0;
};

template <GraphicsApi G>
struct CommandPoolCreateDesc
{
	CommandPoolCreateFlags<G> flags{};
	uint32_t queueFamilyIndex = 0ul;
	bool usePoolReset = true;
};

template <GraphicsApi G>
class CommandPool : public DeviceObject<G>
{
public:
	constexpr CommandPool() noexcept = default;
	CommandPool(
		const std::shared_ptr<Device<G>>& device, CommandPoolCreateDesc<G>&& desc);
	CommandPool(CommandPool&& other) noexcept;
	~CommandPool();

	CommandPool& operator=(CommandPool&& other) noexcept;
	operator auto() const noexcept { return myPool; }

	void swap(CommandPool& rhs) noexcept;
	friend void swap(CommandPool& lhs, CommandPool& rhs) noexcept { lhs.swap(rhs); }

	const auto& getDesc() const noexcept { return myDesc; }

	virtual void reset();

private:
	CommandPool(
		const std::shared_ptr<Device<G>>& device,
		std::tuple<CommandPoolCreateDesc<G>, CommandPoolHandle<G>>&& descAndData);

	CommandPoolCreateDesc<G> myDesc{};
	CommandPoolHandle<G> myPool{};
};

template <GraphicsApi G>
class CommandPoolContext final : public CommandPool<G>
{
	static constexpr uint32_t kCommandBufferLevelCount = 2;

	using CommandBufferListType = std::list<std::tuple<CommandBufferArray<G>, uint64_t>>;

public:
	constexpr CommandPoolContext() noexcept = default;
	CommandPoolContext(
		const std::shared_ptr<Device<G>>& device,
		CommandPoolCreateDesc<G>&& poolDesc);
	CommandPoolContext(CommandPoolContext&& other) noexcept;
	~CommandPoolContext();

	CommandPoolContext& operator=(CommandPoolContext&& other) noexcept;

	virtual void reset();

	void swap(CommandPoolContext& other) noexcept;
	friend void swap(CommandPoolContext& lhs, CommandPoolContext& rhs) noexcept { lhs.swap(rhs); }

	CommandBufferAccessScope<G> commands(const CommandBufferAccessScopeDesc<G>& beginInfo = {});

	uint64_t execute(CommandPoolContext<G>& callee);

	QueueSubmitInfo<G> prepareSubmit(QueueSyncInfo<G>&& syncInfo);

	// these will be called when the GPU has reached the timeline value of the submission (prepareSubmit).
	// useful for ensuring that dependencies are respected when releasing resources. do not remove.
	void addCommandsFinishedCallback(std::function<void(uint64_t)>&& callback);

private:
	CommandBufferAccessScope<G>
	internalBeginScope(const CommandBufferAccessScopeDesc<G>& beginInfo);
	CommandBufferAccessScope<G>
	internalCommands(const CommandBufferAccessScopeDesc<G>& beginInfo) const;
	void internalEndCommands(CommandBufferLevel<G> level);
	void internalEnqueueOnePending(CommandBufferLevel<G> level);
	void internalEnqueueSubmitted(
		CommandBufferListType&& commands, CommandBufferLevel<G> level, uint64_t timelineValue);

	std::array<CommandBufferListType, kCommandBufferLevelCount> myPendingCommands;
	std::array<CommandBufferListType, kCommandBufferLevelCount> mySubmittedCommands;
	std::array<CommandBufferListType, kCommandBufferLevelCount> myFreeCommands;
	std::array<std::optional<CommandBufferAccessScope<G>>, kCommandBufferLevelCount>
		myRecordingCommands;
	std::list<TimelineCallback> myCommandsFinishedCallbacks;
};

#include "command.inl"
