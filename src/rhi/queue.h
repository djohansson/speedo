#pragma once

#include "device.h"
#include "types.h"

#include <any>
#include <memory>
//#include <source_location>
#include <vector>

template <GraphicsBackend B>
struct QueueSyncInfo
{
	std::vector<SemaphoreHandle<B>> waitSemaphores;
	std::vector<Flags<B>> waitDstStageMasks;
	std::vector<uint64_t> waitSemaphoreValues;
	std::vector<SemaphoreHandle<B>> signalSemaphores;
	std::vector<uint64_t> signalSemaphoreValues;
};

template <GraphicsBackend B>
struct QueueSubmitInfo : QueueSyncInfo<B>
{
	std::vector<CommandBufferHandle<B>> commandBuffers;
	uint64_t timelineValue = 0ull;
};

template <GraphicsBackend B>
struct QueuePresentInfo
{
	std::vector<SemaphoreHandle<B>> waitSemaphores;
	std::vector<SwapchainHandle<B>> swapchains;
	std::vector<uint32_t> imageIndices;
	std::vector<Result<B>> results;
	uint64_t timelineValue = 0ull;

	QueuePresentInfo<B>& operator|=(QueuePresentInfo<B>&& other);
	friend QueuePresentInfo<B> operator|(QueuePresentInfo<B>&& lhs, QueuePresentInfo<B>&& rhs);
};

template <GraphicsBackend B>
struct QueueCreateDesc
{
	uint32_t queueIndex = 0ul;
	uint32_t queueFamilyIndex = 0ul;
	std::optional<CommandBufferHandle<B>> tracingEnableInitCmd;
};

struct SourceLocationData
{
	const char* name = nullptr;
	const char* function = nullptr;
	const char* file = nullptr;
	uint32_t line = 0ul;
	uint32_t color = 0ul;
};

template <GraphicsBackend B>
class Queue : public DeviceObject<B>
{
public:
	constexpr Queue() noexcept = default;
	Queue(
		const std::shared_ptr<Device<B>>& device, QueueCreateDesc<B>&& desc);
	Queue(Queue<B>&& other) noexcept;
	~Queue();

	Queue& operator=(Queue&& other) noexcept;
	operator auto() const noexcept { return myQueue; }

	void swap(Queue& rhs) noexcept;
	friend void swap(Queue& lhs, Queue& rhs) noexcept { lhs.swap(rhs); }

	const auto& getDesc() const noexcept { return myDesc; }
	const auto& getLastSubmitTimelineValue() const noexcept { return myLastSubmitTimelineValue; }

	template <typename... Ts>
	void enqueueSubmit(Ts&&... args);
	uint64_t submit();

	template <typename T, typename... Ts>
	void enqueuePresent(T&& first, Ts&&... rest);
	void present();

	void waitIdle() const;

	template <typename T, uint32_t Line>
	std::shared_ptr<void> trace(CommandBufferHandle<B> cmd, const char* function, const char* file);

	void traceCollect(CommandBufferHandle<B> cmd);

private:
	Queue(
		const std::shared_ptr<Device<B>>& device,
		std::tuple<QueueCreateDesc<B>, QueueHandle<B>>&& descAndHandle);

	std::shared_ptr<void>
	internalTrace(CommandBufferHandle<B> cmd, const SourceLocationData& srcLoc);

	QueueCreateDesc<B> myDesc{};
	QueueHandle<B> myQueue{};
	std::vector<QueueSubmitInfo<B>> myPendingSubmits;
	QueuePresentInfo<B> myPendingPresent{};
	std::vector<char> myScratchMemory;
	FenceHandle<B> myFence{};
	std::optional<uint64_t> myLastSubmitTimelineValue;
	std::any myUserData;
};

#if PROFILING_ENABLED
#	define GPU_SCOPE(cmd, queue, name)                                                            \
		struct name##__struct                                                                      \
		{                                                                                          \
			static constexpr const char* getTypeName() { return #name; }                           \
		};                                                                                         \
		auto name##__scope =                                                                       \
			queue.trace<name##__struct, __LINE__>(cmd, __PRETTY_FUNCTION__, __FILE__);
#else
#	define GPU_SCOPE(cmd, queue, name)                                                            \
		{}
#endif

#include "queue.inl"
