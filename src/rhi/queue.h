#pragma once

#include "command.h"
#include "device.h"
#include "types.h"

#include <any>
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
	uint64_t maxTimelineValue = 0ull;

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

enum QueueContextType : uint8_t
{
	QueueContextType_Graphics = 0,
	QueueContextType_Compute = 1,
	QueueContextType_Transfer = 2,
	QueueContextType_SparseBinding = 3,
	QueueContextType_VideoDecode = 5,
	QueueContextType_VideoEncode = 6
};

constexpr std::array<QueueContextType, 6> AllQueueContextTypes{
	QueueContextType_Graphics,
	QueueContextType_Compute,
	QueueContextType_Transfer,
	QueueContextType_SparseBinding,
	QueueContextType_VideoDecode,
	QueueContextType_VideoEncode
};

enum QueueFamilyFlagBits : uint32_t
{
	QueueFamilyFlagBits_Graphics = 1 << QueueContextType_Graphics,
	QueueFamilyFlagBits_Compute = 1 << QueueContextType_Compute,
	QueueFamilyFlagBits_Transfer = 1 << QueueContextType_Transfer,
	QueueFamilyFlagBits_SparseBinding = 1 << QueueContextType_SparseBinding,
	QueueFamilyFlagBits_VideoDecode = 1 << QueueContextType_VideoDecode,
	QueueFamilyFlagBits_VideoEncode = 1 << QueueContextType_VideoEncode
};

template <GraphicsApi G>
struct QueueCreateDesc
{
	uint32_t queueIndex = 0ul;
	uint32_t queueFamilyIndex = 0ul;
#if PROFILING_ENABLED
	CommandBufferHandle<G> tracingInitCmd{};
#endif
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
		const std::shared_ptr<Device<G>>& device,
		QueueCreateDesc<G>&& desc);
	Queue(Queue<G>&& other) noexcept;
	~Queue();

	Queue& operator=(Queue&& other) noexcept;
	operator auto() const noexcept { return myQueue; }

	void swap(Queue& rhs) noexcept;
	friend void swap(Queue& lhs, Queue& rhs) noexcept { lhs.swap(rhs); }

	const auto& getDesc() const noexcept { return myDesc; }

	template <typename... Ts>
	void enqueueSubmit(Ts&&... args);
	QueueHostSyncInfo<G> submit();

	template <typename T, typename... Ts>
	void enqueuePresent(T&& first, Ts&&... rest);
	QueuePresentInfo<G> present();

	void wait(QueueHostSyncInfo<G>&& syncInfo) const;
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

	QueueSubmitInfo<G> prepareSubmit(QueueDeviceSyncInfo<G>&& syncInfo);

	// these will be called when the GPU has reached the timeline value of the submission (prepareSubmit).
	// useful for ensuring that dependencies are respected when releasing resources. do not remove.
	void addCommandsFinishedCallback(std::function<void(uint64_t)>&& callback);

private:
	void internalFlushCommandsFinishedCallbacks(uint32_t timelineValue);

	Queue<G> myQueue;
	std::list<TimelineCallback> myCommandsFinishedCallbacks;
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
