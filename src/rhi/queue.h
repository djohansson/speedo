#pragma once

#include "command.h"
#include "device.h"
#include "semaphore.h"
#include "types.h"

#include <core/task.h>
#include <core/concurrentaccess.h>

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
	 // these will be called after the queue has finished executing the command buffers on the device,
	 // through manually calling the SubmitCallbacks method from the host.
	std::vector<TaskHandle> callbacks;
};

template <GraphicsApi G>
struct QueueHostSyncInfo
{
	std::vector<FenceHandle<G>> fences;
	uint64_t maxTimelineValue = 0ULL; // todo: need to store all semaphores + values?

	QueueHostSyncInfo<G>& operator|=(QueueHostSyncInfo<G>&& other);
	friend QueueHostSyncInfo<G> operator|(QueueHostSyncInfo<G>&& lhs, QueueHostSyncInfo<G>&& rhs);
};

template <GraphicsApi G>
struct QueueSubmitInfo : QueueDeviceSyncInfo<G>
{
	std::vector<CommandBufferHandle<G>> commandBuffers;
	uint64_t timelineValue = 0ULL;
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
	kQueueTypeGraphics = 0,
	kQueueTypeCompute = 1,
	kQueueTypeTransfer = 2,
	kQueueTypeSparseBinding = 3,
	kQueueTypeVideoDecode = 5,
	kQueueTypeVideoEncode = 6
};

constexpr std::array<QueueType, 6> kAllQueueTypes{
	kQueueTypeGraphics,
	kQueueTypeCompute,
	kQueueTypeTransfer,
	kQueueTypeSparseBinding,
	kQueueTypeVideoDecode,
	kQueueTypeVideoEncode};

enum QueueFamilyFlagBits : uint8_t
{
	kQueueFamilyFlagBitsGraphics = 1 << kQueueTypeGraphics,
	kQueueFamilyFlagBitsCompute = 1 << kQueueTypeCompute,
	kQueueFamilyFlagBitsTransfer = 1 << kQueueTypeTransfer,
	kQueueFamilyFlagBitsSparseBinding = 1 << kQueueTypeSparseBinding,
	kQueueFamilyFlagBitsVideoDecode = 1 << kQueueTypeVideoDecode,
	kQueueFamilyFlagBitsVideoEncode = 1 << kQueueTypeVideoEncode
};

template <GraphicsApi G>
struct QueueCreateDesc
{
	uint32_t queueIndex = 0UL;
	uint32_t queueFamilyIndex = 0UL;
};

struct SourceLocationData
{
	const char* name{};
	const char* function{};
	const char* file{};
	uint32_t line = 0UL;
	uint32_t color = 0UL;
};

template <GraphicsApi G>
class Queue final : public DeviceObject<G>
{
public:
	constexpr Queue() noexcept = default;
	Queue(
		const std::shared_ptr<Device<G>>& device,
		CommandPoolCreateDesc<G>&& commandPoolDesc,
		QueueCreateDesc<G>&& queueDesc);
	Queue(Queue<G>&& other) noexcept;
	~Queue() override;

	[[maybe_unused]] Queue& operator=(Queue&& other) noexcept;
	[[nodiscard]] operator auto() const noexcept { return myQueue; }//NOLINT(google-explicit-constructor)

	void Swap(Queue& rhs) noexcept;
	friend void Swap(Queue& lhs, Queue& rhs) noexcept { lhs.Swap(rhs); }

	[[nodiscard]] const auto& GetDesc() const noexcept { return myDesc; }

	template <typename T, typename... Ts>
	void EnqueueSubmit(T&& first, Ts&&... rest);
	[[nodiscard]] QueueHostSyncInfo<G> Submit();

	template <typename T, typename... Ts>
	void EnqueuePresent(T&& first, Ts&&... rest);
	[[maybe_unused]] QueuePresentInfo<G> Present();

	void Execute(uint8_t level, uint64_t timelineValue);

	void Wait(QueueHostSyncInfo<G>&& syncInfo) const;
	void WaitIdle() const;
	
	bool SubmitCallbacks(TaskExecutor& executor, uint64_t timelineValue);

	[[nodiscard]] auto& GetPool() noexcept { return myPool; }
	[[nodiscard]] const auto& GetPool() const noexcept { return myPool; }

#if (PROFILING_LEVEL > 0)
	template <SourceLocationData Location>
	inline std::shared_ptr<void> GpuScope(CommandBufferHandle<G> cmd);
	void GpuScopeCollect(CommandBufferHandle<G> cmd);
#endif

private:
	Queue(
		const std::shared_ptr<Device<G>>& device,
		CommandPoolCreateDesc<G>&& commandPoolDesc,
		std::tuple<QueueCreateDesc<G>, QueueHandle<G>>&& descAndHandle);

	[[nodiscard]] QueueSubmitInfo<G> InternalPrepareSubmit(QueueDeviceSyncInfo<G>&& syncInfo);

#if (PROFILING_LEVEL > 0)
	[[nodiscard]] std::shared_ptr<void> InternalGpuScope(CommandBufferHandle<G> cmd, const SourceLocationData& srcLoc);
#endif

	QueueCreateDesc<G> myDesc{};
	QueueHandle<G> myQueue{};
	CommandPool<G> myPool;
	std::vector<QueueSubmitInfo<G>> myPendingSubmits;
	QueuePresentInfo<G> myPendingPresent{};
	std::vector<char> myScratchMemory;
	using TimelineCallbackData = std::tuple<std::vector<TaskHandle>, uint64_t>;
	ConcurrentQueue<TimelineCallbackData> myTimelineCallbacks;

#if (PROFILING_LEVEL > 0)
	void* myProfilingContext = nullptr;
#endif
};

template <GraphicsApi G>
using QueueContext = std::pair<Queue<G>, QueueHostSyncInfo<G>>;

template <GraphicsApi G>
using QueueTimelineContext = std::tuple<Semaphore<G>, CopyableAtomic<uint64_t>, ConcurrentAccess<std::vector<QueueContext<G>>>>;

#if (PROFILING_LEVEL > 0)
#	define GPU_SCOPE(cmd, queue, tag)                                                             \
		auto tag##__scope =                                                                        \
			(queue)                                                                                \
				.GpuScope<SourceLocationData{                                                      \
					.name = std_extra::make_string_literal<#tag>().data(),                         \
					.function = std_extra::make_string_literal<__PRETTY_FUNCTION__>().data(),      \
					.file = std_extra::make_string_literal<__FILE__>().data(),                     \
					.line = __LINE__}>(cmd);
#	define GPU_SCOPE_COLLECT(cmd, queue)                                                          \
		{                                                                                          \
			(queue).GpuScopeCollect(cmd);                                                          \
		}
#else
#	define GPU_SCOPE(cmd, queue, tag) {}
#	define GPU_SCOPE_COLLECT(cmd, queue) {}
#endif

#include "queue.inl"
