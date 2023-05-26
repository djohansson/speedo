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

template <GraphicsBackend B>
struct CommandBufferArrayCreateDesc
{
	CommandPoolHandle<B> pool{};
	CommandBufferLevel<B> level{};
	bool useBufferReset = false;
};

template <GraphicsBackend B>
class CommandBufferArray : public DeviceObject<B>
{
	static constexpr uint32_t kHeadBitCount = 2;
	static constexpr size_t kCommandBufferCount = (1 << kHeadBitCount);

public:
	constexpr CommandBufferArray() noexcept = default;
	CommandBufferArray(
		const std::shared_ptr<Device<B>>& device,
		CommandBufferArrayCreateDesc<B>&& desc);
	CommandBufferArray(CommandBufferArray&& other) noexcept;
	~CommandBufferArray();

	CommandBufferArray& operator=(CommandBufferArray&& other) noexcept;
	CommandBufferHandle<B> operator[](uint8_t index) const { return myArray[index]; }

	void swap(CommandBufferArray& rhs) noexcept;
	friend void swap(CommandBufferArray& lhs, CommandBufferArray& rhs) noexcept { lhs.swap(rhs); }

	const auto& getDesc() const noexcept { return myDesc; }

	static constexpr auto capacity() { return kCommandBufferCount; }

	uint8_t begin(const CommandBufferBeginInfo<B>& beginInfo);
	void end(uint8_t index);

	void reset();

	uint8_t head() const { return myBits.head; }
	const CommandBufferHandle<B>* data() const
	{
		assert(!recordingFlags());
		return myArray.data();
	}

	bool recording(uint8_t index) const { return myBits.recordingFlags & (1 << index); }
	uint8_t recordingFlags() const noexcept { return myBits.recordingFlags; }

	bool full() const { return (head() + 1) >= capacity(); }

private:
	CommandBufferArray(
		const std::shared_ptr<Device<B>>& device,
		std::tuple<
			CommandBufferArrayCreateDesc<B>,
			std::array<CommandBufferHandle<B>, kCommandBufferCount>>&& descAndData);

	CommandBufferArrayCreateDesc<B> myDesc{};
	std::array<CommandBufferHandle<B>, kCommandBufferCount> myArray{};
	struct Bits
	{
		uint8_t head : kHeadBitCount;
		uint8_t recordingFlags : kCommandBufferCount;
	} myBits{0, 0};
};

template <GraphicsBackend B>
struct CommandBufferAccessScopeDesc : CommandBufferBeginInfo<B>
{
	CommandBufferAccessScopeDesc(bool scopedBeginEnd = true);
	CommandBufferAccessScopeDesc(const CommandBufferAccessScopeDesc<B>& other);

	CommandBufferAccessScopeDesc<B>& operator=(const CommandBufferAccessScopeDesc<B>& other);
	bool operator==(const CommandBufferAccessScopeDesc<B>& other) const;

	CommandBufferLevel<B> level{};
	CommandBufferInheritanceInfo<Vk> inheritance{};
	bool scopedBeginEnd = true;
};

template <GraphicsBackend B>
class CommandBufferAccessScope : Nondynamic
{
public:
	constexpr CommandBufferAccessScope() noexcept = default;
	CommandBufferAccessScope(
		CommandBufferArray<B>* array, const CommandBufferAccessScopeDesc<B>& beginInfo);
	CommandBufferAccessScope(const CommandBufferAccessScope& other);
	CommandBufferAccessScope(CommandBufferAccessScope&& other) noexcept;
	~CommandBufferAccessScope();

	CommandBufferAccessScope<B>& operator=(CommandBufferAccessScope<B> other);
	operator auto() const { return (*myArray)[myIndex]; }

	void swap(CommandBufferAccessScope<B>& rhs) noexcept;
	friend void swap(CommandBufferAccessScope<B>& lhs, CommandBufferAccessScope<B>& rhs) noexcept
	{
		lhs.swap(rhs);
	}

	const auto& getDesc() const { return myDesc; }

	void begin() { myIndex = myArray->begin(myDesc); }
	void end() const { myArray->end(myIndex); }

private:
	CommandBufferAccessScopeDesc<B> myDesc{};
	std::shared_ptr<uint32_t> myRefCount;
	CommandBufferArray<B>* myArray = nullptr;
	uint8_t myIndex = 0;
};

template <GraphicsBackend B>
struct CommandPoolCreateDesc
{
	CommandPoolCreateFlags<B> flags{};
	uint32_t queueFamilyIndex = 0ul;
	bool usePoolReset = true;
};

template <GraphicsBackend B>
class CommandPool : public DeviceObject<B>
{
public:
	constexpr CommandPool() noexcept = default;
	CommandPool(
		const std::shared_ptr<Device<B>>& device, CommandPoolCreateDesc<B>&& desc);
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
		const std::shared_ptr<Device<B>>& device,
		std::tuple<CommandPoolCreateDesc<B>, CommandPoolHandle<B>>&& descAndData);

	CommandPoolCreateDesc<B> myDesc{};
	CommandPoolHandle<B> myPool{};
};

template <GraphicsBackend B>
class CommandPoolContext : public CommandPool<B>
{
	static constexpr uint32_t kCommandBufferLevelCount = 2;

	using CommandBufferListType = std::list<std::tuple<CommandBufferArray<B>, uint64_t>>;

public:
	constexpr CommandPoolContext() noexcept = default;
	CommandPoolContext(
		const std::shared_ptr<Device<B>>& device,
		CommandPoolCreateDesc<B>&& poolDesc);
	CommandPoolContext(CommandPoolContext&& other) noexcept;
	~CommandPoolContext();

	CommandPoolContext& operator=(CommandPoolContext&& other) noexcept;

	virtual void reset() final;

	void swap(CommandPoolContext& other) noexcept;
	friend void swap(CommandPoolContext& lhs, CommandPoolContext& rhs) noexcept { lhs.swap(rhs); }

	CommandBufferAccessScope<B> commands(const CommandBufferAccessScopeDesc<B>& beginInfo = {});

	uint64_t execute(CommandPoolContext<B>& callee);

	QueueSubmitInfo<B> prepareSubmit(QueueSyncInfo<B>&& syncInfo);

	// these will be called when the GPU has reached the timeline value of the submission (prepareSubmit).
	// useful for ensuring that dependencies are respected when releasing resources. do not remove.
	void addCommandsFinishedCallback(std::function<void(uint64_t)>&& callback);

private:
	CommandBufferAccessScope<B>
	internalBeginScope(const CommandBufferAccessScopeDesc<B>& beginInfo);
	CommandBufferAccessScope<B>
	internalCommands(const CommandBufferAccessScopeDesc<B>& beginInfo) const;
	void internalEndCommands(CommandBufferLevel<B> level);
	void internalEnqueueOnePending(CommandBufferLevel<B> level);
	void internalEnqueueSubmitted(
		CommandBufferListType&& commands, CommandBufferLevel<B> level, uint64_t timelineValue);

	std::array<CommandBufferListType, kCommandBufferLevelCount> myPendingCommands;
	std::array<CommandBufferListType, kCommandBufferLevelCount> mySubmittedCommands;
	std::array<CommandBufferListType, kCommandBufferLevelCount> myFreeCommands;
	std::array<std::optional<CommandBufferAccessScope<B>>, kCommandBufferLevelCount>
		myRecordingCommands;
	std::list<TimelineCallback> myCommandsFinishedCallbacks;
};

#include "command.inl"
