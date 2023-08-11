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
	const char* name{};
	const char* function{};
	const char* file{};
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

#if PROFILING_ENABLED
	template <SourceLocationData Location>
	FORCE_INLINE std::shared_ptr<void> gpuScope(CommandBufferHandle<B> cmd);
	void gpuScopeCollect(CommandBufferHandle<B> cmd);
#endif

private:
	Queue(
		const std::shared_ptr<Device<B>>& device,
		std::tuple<QueueCreateDesc<B>, QueueHandle<B>>&& descAndHandle);

#if PROFILING_ENABLED
	std::shared_ptr<void>
	internalGpuScope(CommandBufferHandle<B> cmd, const SourceLocationData& srcLoc);
#endif

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
#	define GPU_SCOPE(cmd, queue, tag)										\
		auto tag##__scope =													\
			queue.gpuScope<													\
				SourceLocationData{											\
					.name = string_literal<#tag>(),  						\
					.function = string_literal<__PRETTY_FUNCTION__>(),		\
					.file = string_literal<__FILE__>(),						\
					.line = __LINE__}>(cmd);
#	define GPU_SCOPE_COLLECT(cmd, queue) { queue.gpuScopeCollect(cmd); }
#else
#	define GPU_SCOPE(cmd, queue, tag) {}
#	define GPU_SCOPE_COLLECT(cmd, queue) {}
#endif

#include "queue.inl"
