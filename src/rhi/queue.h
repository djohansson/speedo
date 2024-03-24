#pragma once

#include "command.h"
#include "device.h"
#include "types.h"

#include <any>
#include <deque>
#include <memory>
//#include <source_location>
#include <vector>

template <GraphicsApi G>
struct QueueDeviceSyncInfo
{
	std::vector<SemaphoreHandle<G>> waitSemaphores;
	std::vector<Flags<G>> waitDstStageMasks;
	std::vector<uint64_t> waitSemaphoreValues;
	std::vector<SemaphoreHandle<G>> signalSemaphores;
	std::vector<uint64_t> signalSemaphoreValues;
};

template <GraphicsApi G>
struct QueueHostSyncInfo
{
	std::vector<FenceHandle<G>> fences;
	uint64_t maxTimelineValue = 0ull; // todo: need to store all semaphores + values

	QueueHostSyncInfo<G>& operator|=(QueueHostSyncInfo<G>&& other);
	friend QueueHostSyncInfo<G> operator|(QueueHostSyncInfo<G>&& lhs, QueueHostSyncInfo<G>&& rhs);
};

template <GraphicsApi G>
struct QueueSubmitInfo : QueueDeviceSyncInfo<G>
{
	std::vector<CommandBufferHandle<G>> commandBuffers;
	uint64_t timelineValue = 0ull;
};

template <GraphicsApi G>
struct QueuePresentInfo : QueueHostSyncInfo<G>
{
	std::vector<SemaphoreHandle<G>> waitSemaphores;
	std::vector<SwapchainHandle<G>> swapchains;
	std::vector<uint32_t> imageIndices;
	std::vector<Result<G>> results;

	QueuePresentInfo<G>& operator|=(QueuePresentInfo<G>&& other);
	friend QueuePresentInfo<G> operator|(QueuePresentInfo<G>&& lhs, QueuePresentInfo<G>&& rhs);
};

enum QueueType : uint8_t
{
	QueueType_Graphics = 0,
	QueueType_Compute = 1,
	QueueType_Transfer = 2,
	QueueType_SparseBinding = 3,
	QueueType_VideoDecode = 5,
	QueueType_VideoEncode = 6
};

constexpr std::array<QueueType, 6> AllQueueTypes{
	QueueType_Graphics,
	QueueType_Compute,
	QueueType_Transfer,
	QueueType_SparseBinding,
	QueueType_VideoDecode,
	QueueType_VideoEncode
};

enum QueueFamilyFlagBits : uint32_t
{
	QueueFamilyFlagBits_Graphics = 1 << QueueType_Graphics,
	QueueFamilyFlagBits_Compute = 1 << QueueType_Compute,
	QueueFamilyFlagBits_Transfer = 1 << QueueType_Transfer,
	QueueFamilyFlagBits_SparseBinding = 1 << QueueType_SparseBinding,
	QueueFamilyFlagBits_VideoDecode = 1 << QueueType_VideoDecode,
	QueueFamilyFlagBits_VideoEncode = 1 << QueueType_VideoEncode
};

template <GraphicsApi G>
struct QueueCreateDesc
{
	uint32_t queueIndex = 0ul;
	uint32_t queueFamilyIndex = 0ul;
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
class Queue final : public CommandPool<G>
{
	using TimelineCallback = std::tuple<uint64_t, std::function<void(uint64_t)>>;

public:
	constexpr Queue() noexcept = default;
	Queue(
		const std::shared_ptr<Device<G>>& device,
		CommandPoolCreateDesc<G>&& commandPoolDesc,
		QueueCreateDesc<G>&& queueDesc);
	Queue(Queue<G>&& other) noexcept;
	~Queue();

	Queue& operator=(Queue&& other) noexcept;
	operator auto() const noexcept { return myQueue; }

	void swap(Queue& rhs) noexcept;
	friend void swap(Queue& lhs, Queue& rhs) noexcept { lhs.swap(rhs); }

	const auto& getDesc() const noexcept { return myDesc; }

	template <typename T, typename... Ts>
	void enqueueSubmit(T&& first, Ts&&... rest);
	QueueHostSyncInfo<G> submit();

	template <typename T, typename... Ts>
	void enqueuePresent(T&& first, Ts&&... rest);
	QueuePresentInfo<G> present();

	uint64_t execute(uint32_t index);

	void wait(QueueHostSyncInfo<G>&& syncInfo) const;
	void waitIdle() const;
	
	void addTimelineCallback(TimelineCallback&& callback);
	bool processTimelineCallbacks(uint64_t timelineValue);

#if PROFILING_ENABLED
	template <SourceLocationData Location>
	inline std::shared_ptr<void> gpuScope(CommandBufferHandle<G> cmd);
	void gpuScopeCollect(CommandBufferHandle<G> cmd);
#endif

private:
	Queue(
		const std::shared_ptr<Device<G>>& device,
		CommandPoolCreateDesc<G>&& commandPoolDesc,
		std::tuple<QueueCreateDesc<G>, QueueHandle<G>>&& descAndHandle);

	QueueSubmitInfo<G> internalPrepareSubmit(QueueDeviceSyncInfo<G>&& syncInfo);

#if PROFILING_ENABLED
	std::shared_ptr<void>
	internalGpuScope(CommandBufferHandle<G> cmd, const SourceLocationData& srcLoc);
#endif

	QueueCreateDesc<G> myDesc{};
	QueueHandle<G> myQueue{};
	std::vector<QueueSubmitInfo<G>> myPendingSubmits;
	QueuePresentInfo<G> myPendingPresent{};
	std::vector<char> myScratchMemory;
	std::any myUserData;
	std::deque<TimelineCallback> myTimelineCallbacks;
};

#if PROFILING_ENABLED
#	define GPU_SCOPE(cmd, queue, tag)											\
		auto tag##__scope =														\
			queue.gpuScope<														\
				SourceLocationData{												\
					.name = string_literal<#tag>().data(),						\
					.function = string_literal<__PRETTY_FUNCTION__>().data(),	\
					.file = string_literal<__FILE__>().data(),					\
					.line = __LINE__}>(cmd);
#	define GPU_SCOPE_COLLECT(cmd, queue) { queue.gpuScopeCollect(cmd); }
#else
#	define GPU_SCOPE(cmd, queue, tag) {}
#	define GPU_SCOPE_COLLECT(cmd, queue) {}
#endif

#include "queue.inl"
