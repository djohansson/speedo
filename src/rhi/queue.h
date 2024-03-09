#pragma once

#include "command.h"
#include "device.h"
#include "types.h"

#include <any>
#include <memory>
//#include <source_location>
#include <vector>

template <GraphicsApi G>
struct QueueSyncInfo
{
	std::vector<SemaphoreHandle<G>> waitSemaphores;
	std::vector<Flags<G>> waitDstStageMasks;
	std::vector<uint64_t> waitSemaphoreValues;
	std::vector<SemaphoreHandle<G>> signalSemaphores;
	std::vector<uint64_t> signalSemaphoreValues;
};

template <GraphicsApi G>
struct QueueSubmitInfo : QueueSyncInfo<G>
{
	std::vector<CommandBufferHandle<G>> commandBuffers;
	uint64_t timelineValue = 0ull;
};

template <GraphicsApi G>
struct QueuePresentInfo
{
	std::vector<SemaphoreHandle<G>> waitSemaphores;
	std::vector<SwapchainHandle<G>> swapchains;
	std::vector<uint32_t> imageIndices;
	std::vector<Result<G>> results;
	uint64_t timelineValue = 0ull;

	QueuePresentInfo<G>& operator|=(QueuePresentInfo<G>&& other);
	friend QueuePresentInfo<G> operator|(QueuePresentInfo<G>&& lhs, QueuePresentInfo<G>&& rhs);
};

enum QueueContextType : uint8_t
{
	QueueContextType_Graphics,
	QueueContextType_Compute,
	QueueContextType_Transfer,
	
	QueueContextType_Count
};

template <GraphicsApi G>
struct QueueCreateDesc
{
	uint32_t queueIndex = 0ul;
	uint32_t queueFamilyIndex = 0ul;
	bool tracingEnable = false;
};

struct SourceLocationData
{
	const char* name{};
	const char* function{};
	const char* file{};
	uint32_t line = 0ul;
	uint32_t color = 0ul;
};

template <GraphicsApi G>
class Queue final : public DeviceObject<G>
{
public:
	constexpr Queue() noexcept = default;
	Queue(
		const std::shared_ptr<Device<G>>& device, QueueCreateDesc<G>&& desc);
	Queue(Queue<G>&& other) noexcept;
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
	inline std::shared_ptr<void> gpuScope(CommandBufferHandle<G> cmd);
	void gpuScopeCollect(CommandBufferHandle<G> cmd);
#endif

private:
	Queue(
		const std::shared_ptr<Device<G>>& device,
		std::tuple<QueueCreateDesc<G>, QueueHandle<G>>&& descAndHandle);

#if PROFILING_ENABLED
	std::shared_ptr<void>
	internalGpuScope(CommandBufferHandle<G> cmd, const SourceLocationData& srcLoc);
#endif

	QueueCreateDesc<G> myDesc{};
	QueueHandle<G> myQueue{};
	std::vector<QueueSubmitInfo<G>> myPendingSubmits;
	QueuePresentInfo<G> myPendingPresent{};
	std::vector<char> myScratchMemory;
	FenceHandle<G> myFence{};
	std::optional<uint64_t> myLastSubmitTimelineValue;
	std::any myUserData;
};

template <GraphicsApi G>
class QueueContext final : public CommandPool<G>
{
public:
	constexpr QueueContext() noexcept = default;
	QueueContext(
		const std::shared_ptr<Device<G>>& device,
		CommandPoolCreateDesc<G>&& commandPoolDesc,
		QueueCreateDesc<G>&& queueDesc);
	QueueContext(QueueContext<G>&& other) noexcept;
	~QueueContext();

	QueueContext& operator=(QueueContext&& other) noexcept;

	void swap(QueueContext& rhs) noexcept;
	friend void swap(QueueContext& lhs, QueueContext& rhs) noexcept { lhs.swap(rhs); }

	const auto& queue() const noexcept { return myQueue; }
	auto& queue() noexcept { return myQueue; }

	uint64_t execute(uint32_t index);

	QueueSubmitInfo<G> prepareSubmit(QueueSyncInfo<G>&& syncInfo);

	// these will be called when the GPU has reached the timeline value of the submission (prepareSubmit).
	// useful for ensuring that dependencies are respected when releasing resources. do not remove.
	void addCommandsFinishedCallback(std::function<void(uint64_t)>&& callback);

private:
	void internalFlushCommandsFinishedCallbacks(uint32_t timelineValue);

	Queue<G> myQueue;
	std::list<TimelineCallback> myCommandsFinishedCallbacks;
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
