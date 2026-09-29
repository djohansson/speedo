#pragma once

#include <rhi/command.h>
#include <rhi/object.h>
#include <rhi/fence.h>
#include <rhi/capi.h>
#include <rhi/semaphore.h>
#include <rhi/types.h>

#include <core/taskexecutor.h>
#include <core/circularcontainer.h>
#include <core/concurrentaccess.h>
#include <core/std_extra.h>
#include <core/upgradablesharedmutex.h>

#include <cstdint>
#include <memory>
//#include <source_location>
#include <vector>

namespace rhi
{

template <GraphicsApi G>
struct QueueFamilyDesc
{
	uint32_t queueCount = 0UL;
	uint32_t flags = 0UL;
	uint32_t timestampValidBits = 0UL;
	Extent3d<G> minImageTransferGranularity{};
};

template <GraphicsApi G>
struct QueueDeviceSyncInfo
{
	std::vector<SemaphoreHandle<G>> waitSemaphores;
	std::vector<Flags<G>> waitDstStageMasks;
	std::vector<uint64_t> waitSemaphoreValues;
	std::vector<SemaphoreHandle<G>> signalSemaphores;
	std::vector<uint64_t> signalSemaphoreValues;
	// These will be passed on and stored internally in the queue to be called after the queue has finished executing the command buffers on the device.
	// They will then be called and deleted through manually calling the Queue::SubmitCallbacks from the host.
	std::vector<core::TaskHandle> callbacks;
};

template <GraphicsApi G>
struct QueueHostSyncInfo
{
	std::vector<Fence<G>> fences;
	uint64_t maxTimelineValue = 0ULL;

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
struct QueuePresentInfo
{
	std::vector<SemaphoreHandle<G>> waitSemaphores;
	std::vector<SwapchainHandle<G>> swapchains;
	std::vector<uint32_t> imageIndices;
	std::vector<Result<G>> results;
	std::vector<uint64_t> presentIds;

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
struct QueueCreateDesc final : DeviceObjectCreateDesc<G>
{
	uint32_t queueIndex = 0UL;
	uint32_t queueFamilyIndex : 27;
	uint32_t levelCount : 4;
	uint32_t supportsProfiling : 1;
};

template <GraphicsApi G>
class Queue;

template <GraphicsApi G>
struct ObjectTraits<Queue<G>>
{
	using CreateDescType = QueueCreateDesc<G>;
};

template <GraphicsApi G>
class Queue final : public DeviceObject<Queue<G>>
{
public:
	using SuperType = DeviceObject<Queue<G>>;
	using CreateDescType = ObjectTraits<Queue<G>>::CreateDescType;

	constexpr Queue() noexcept = default;
	explicit Queue(CreateDescType&& queueDesc);
	Queue(Queue<G>&& other) noexcept;
	~Queue();

	[[maybe_unused]] Queue& operator=(Queue&& other) noexcept;
	[[nodiscard]] operator auto() const noexcept { return myQueue; }//NOLINT(google-explicit-constructor)

	void Swap(Queue& rhs) noexcept;
	friend void Swap(Queue& lhs, Queue& rhs) noexcept { lhs.Swap(rhs); }

	template <typename T, typename... Ts>
	void EnqueueSubmit(T&& first, Ts&&... rest);
	[[nodiscard]] QueueHostSyncInfo<G> Submit();

	template <typename T, typename... Ts>
	void EnqueuePresent(T&& first, Ts&&... rest);
	[[maybe_unused]] QueueHostSyncInfo<G> Present();

	void Execute(uint8_t level, uint64_t timelineValue);

	void WaitIdle() const;
	
	bool SubmitCallbacks(core::TaskExecutor& executor, uint64_t timelineValue) const;

	[[nodiscard]] auto& GetPool() noexcept { return myPools[0]; }
	[[nodiscard]] const auto& GetPool() const noexcept { return myPools[0]; }

	void SwapAndResetPool();

	template <SourceLocationData Location>
	[[nodiscard]] std::shared_ptr<void> CreateGpuScope(CommandBufferHandle<G> cmd);
	void CollectGpuScope(CommandBufferHandle<G> cmd);

private:
	Queue(
		CreateDescType&& queueDesc,
		QueueHandle<G>&& handle);

	[[nodiscard]] QueueSubmitInfo<G> InternalPrepareSubmit(QueueDeviceSyncInfo<G>&& syncInfo);
	[[nodiscard]] std::shared_ptr<void> InternalGpuScope(CommandBufferHandle<G> cmd, const SourceLocationData& srcLoc);

	QueueHandle<G> myQueue{};
	std::array<CommandPool<G>, 2> myPools;
	std::vector<QueueSubmitInfo<G>> myPendingSubmits;
	QueuePresentInfo<G> myPendingPresent{};
	std::vector<char> myScratchMemory;
	using TimelineCallbackData = std::tuple<std::vector<core::TaskHandle>, uint64_t>;
	mutable core::ConcurrentQueue<TimelineCallbackData> myTimelineCallbacks;

#if (SPEEDO_PROFILING_LEVEL > 0)
	void* myProfilingContext = nullptr;
#endif
};

template <GraphicsApi G>
using QueueContext = std::pair<Queue<G>, QueueHostSyncInfo<G>>;

template <GraphicsApi G>
struct QueueTimelineContextData
{
	Semaphore<G> semaphore;
	uint64_t timeline = 0ULL;
	uint32_t queueFamilyIndex = 0UL;
	core::CircularContainer<QueueContext<G>> queues;
};

template <GraphicsApi G>
using QueueTimelineContext = core::ConcurrentAccess<std::shared_ptr<QueueTimelineContextData<G>>>;

#if (SPEEDO_PROFILING_LEVEL > 0)
#	define GPU_SCOPE(cmd, queue, tag) auto COUNTED_VAR_DECLARE(tag) = (queue).CreateGpuScope<SOURCE_LOCATION_DATA(tag)>(cmd)
#	define GPU_SCOPE_COLLECT(cmd, queue) (queue).CollectGpuScope(cmd)
#else
#	define GPU_SCOPE(cmd, queue, tag) {}
#	define GPU_SCOPE_COLLECT(cmd, queue) {}
#endif

} // namespace rhi

#include "queue.inl"
