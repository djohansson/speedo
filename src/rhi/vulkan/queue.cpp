#include "../queue.h"
#include "../fence.h"
#include "utils.h"

#include <tracy/TracyC.h>
#include <tracy/TracyVulkan.hpp>

template <>
bool Queue<kVk>::SubmitCallbacks(TaskExecutor& executor, uint64_t timelineValue)
{
	ZoneScopedN("Queue::SubmitCallbacks");

	TimelineCallbackData callbackData;
	while (myTimelineCallbacks.try_dequeue(callbackData))
	{
		auto& [callbackVector, commandBufferTimelineValue] = callbackData;

		if (commandBufferTimelineValue > timelineValue)
		{
			myTimelineCallbacks.enqueue(std::move(callbackData));
			return false;
		}

		executor.Submit(std::span(callbackVector.data(), callbackVector.size()));
	}

	return true;
}

template <>
Queue<kVk>::Queue(
	const std::shared_ptr<Device<kVk>>& device,
	CommandPoolCreateDesc<kVk>&& commandPoolDesc,
	std::tuple<QueueCreateDesc<kVk>, QueueHandle<kVk>>&& descAndHandle)
	: DeviceObject(
		  device,
		  {"_Queue"},
		  1,
		  VK_OBJECT_TYPE_QUEUE,
		  reinterpret_cast<uint64_t*>(&std::get<1>(descAndHandle)))
	, myDesc(std::forward<QueueCreateDesc<kVk>>(std::get<0>(descAndHandle)))
	, myQueue(std::get<1>(descAndHandle))
	, myPool(device, std::forward<CommandPoolCreateDesc<kVk>>(commandPoolDesc))
{
	using namespace tracy;

	static_assert(
		static_cast<uint32_t>(kQueueFamilyFlagBitsGraphics) ==
		static_cast<uint32_t>(VK_QUEUE_GRAPHICS_BIT));
	static_assert(
		static_cast<uint32_t>(kQueueFamilyFlagBitsCompute) ==
		static_cast<uint32_t>(VK_QUEUE_COMPUTE_BIT));
	static_assert(
		static_cast<uint32_t>(kQueueFamilyFlagBitsTransfer) ==
		static_cast<uint32_t>(VK_QUEUE_TRANSFER_BIT));
	static_assert(
		static_cast<uint32_t>(kQueueFamilyFlagBitsSparseBinding) ==
		static_cast<uint32_t>(VK_QUEUE_SPARSE_BINDING_BIT));
	static_assert(
		static_cast<uint32_t>(kQueueFamilyFlagBitsVideoDecode) ==
		static_cast<uint32_t>(VK_QUEUE_VIDEO_DECODE_BIT_KHR));
	static_assert(
		static_cast<uint32_t>(kQueueFamilyFlagBitsVideoEncode) ==
		static_cast<uint32_t>(VK_QUEUE_VIDEO_ENCODE_BIT_KHR));

#if (PROFILING_LEVEL > 0)
	if (myPool.GetDesc().supportsProfiling)
		myProfilingContext = CreateVkContext(
			device->GetPhysicalDevice(),
			*device,
			myQueue,
			myPool.Commands(CommandBufferAccessScopeDesc<kVk>(false)),
			nullptr,
			nullptr);
#endif
}

template <>
Queue<kVk>::Queue(
	const std::shared_ptr<Device<kVk>>& device,
	CommandPoolCreateDesc<kVk>&& commandPoolDesc,
	QueueCreateDesc<kVk>&& queueDesc)
	: Queue(
		device,
		std::forward<CommandPoolCreateDesc<kVk>>(commandPoolDesc),
		std::make_tuple(
			std::forward<QueueCreateDesc<kVk>>(queueDesc),
			[&device, &queueDesc]
			{
				QueueHandle<kVk> queue;
				vkGetDeviceQueue(*device, queueDesc.queueFamilyIndex, queueDesc.queueIndex, &queue);
				return queue;
			}()))
{}

template <>
Queue<kVk>::Queue(Queue<kVk>&& other) noexcept
	: DeviceObject(std::forward<Queue<kVk>>(other))
	, myDesc(std::exchange(other.myDesc, {}))
	, myQueue(std::exchange(other.myQueue, {}))
	, myPool(std::exchange(other.myPool, {}))
	, myPendingSubmits(std::exchange(other.myPendingSubmits, {}))
	, myScratchMemory(std::exchange(other.myScratchMemory, {}))
#if (PROFILING_LEVEL > 0)
	, myProfilingContext(std::exchange(other.myProfilingContext, {}))
#endif
{}

template <>
Queue<kVk>::~Queue()
{
	using namespace tracy;

	ZoneScopedN("Queue::~Queue()");

#if (PROFILING_LEVEL > 0)
	if (myProfilingContext)
		DestroyVkContext(static_cast<TracyVkCtx>(myProfilingContext));
#endif

	ASSERT(myTimelineCallbacks.size_approx() == 0);
}

template <>
Queue<kVk>& Queue<kVk>::operator=(Queue<kVk>&& other) noexcept
{
	DeviceObject::operator=(std::forward<Queue<kVk>>(other));
	myDesc = std::exchange(other.myDesc, {});
	myQueue = std::exchange(other.myQueue, {});
	myPool = std::exchange(other.myPool, {});
	myPendingSubmits = std::exchange(other.myPendingSubmits, {});
	myScratchMemory = std::exchange(other.myScratchMemory, {});
	std::swap(myTimelineCallbacks, other.myTimelineCallbacks);
	decltype(other.myTimelineCallbacks) tmp;
	std::swap(other.myTimelineCallbacks, tmp);
#if (PROFILING_LEVEL > 0)
	myProfilingContext = std::exchange(other.myProfilingContext, {});
#endif
	return *this;
}

template <>
void Queue<kVk>::Swap(Queue& other) noexcept
{
	DeviceObject::Swap(other);
	std::swap(myDesc, other.myDesc);
	std::swap(myQueue, other.myQueue);
	std::swap(myPool, other.myPool);
	std::swap(myPendingSubmits, other.myPendingSubmits);
	std::swap(myScratchMemory, other.myScratchMemory);
	std::swap(myTimelineCallbacks, other.myTimelineCallbacks);
#if (PROFILING_LEVEL > 0)
	std::swap(myProfilingContext, other.myProfilingContext);
#endif
}

#if (PROFILING_LEVEL > 0)
template <>
void Queue<kVk>::GpuScopeCollect(CommandBufferHandle<kVk> cmd)
{
	if (myProfilingContext)
		TracyVkCollect(static_cast<TracyVkCtx>(myProfilingContext), cmd);
}

template <>
std::shared_ptr<void>
Queue<kVk>::InternalGpuScope(CommandBufferHandle<kVk> cmd, const SourceLocationData& srcLoc)
{
	static_assert(sizeof(SourceLocationData) == sizeof(tracy::SourceLocationData));
	// static_assert(offsetof(SourceLocationData, name) == offsetof(tracy::SourceLocationData, name));
	// static_assert(offsetof(SourceLocationData, function) == offsetof(tracy::SourceLocationData, function));
	// static_assert(offsetof(SourceLocationData, file) == offsetof(tracy::SourceLocationData, file));
	// static_assert(offsetof(SourceLocationData, line) == offsetof(tracy::SourceLocationData, line));
	// static_assert(offsetof(SourceLocationData, color) == offsetof(tracy::SourceLocationData, color));

	if (myProfilingContext)
	{
		return std::make_shared<tracy::VkCtxScope>(
			static_cast<TracyVkCtx>(myProfilingContext),
			reinterpret_cast<const tracy::SourceLocationData*>(&srcLoc),
			cmd,
			true);
	}

	return {};
}
#endif

template <>
QueueHostSyncInfo<kVk> Queue<kVk>::Submit()
{
	ZoneScopedN("Queue::submit");

	if (myPendingSubmits.empty())
		return {};

	myScratchMemory.resize(
		(sizeof(SubmitInfo<kVk>) + sizeof(TimelineSemaphoreSubmitInfo<kVk>)) *
		myPendingSubmits.size());

	auto* timelineBegin =
		reinterpret_cast<TimelineSemaphoreSubmitInfo<kVk>*>(myScratchMemory.data());
	auto* timelinePtr = timelineBegin;

	uint64_t maxTimelineValue = 0ULL;

	for (const auto& pendingSubmit : myPendingSubmits)
	{
		auto& timelineInfo = *(timelinePtr++);

		timelineInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
		timelineInfo.pNext = nullptr;
		timelineInfo.waitSemaphoreValueCount = pendingSubmit.waitSemaphoreValues.size();
		timelineInfo.pWaitSemaphoreValues = pendingSubmit.waitSemaphoreValues.data();
		timelineInfo.signalSemaphoreValueCount = pendingSubmit.signalSemaphoreValues.size();
		timelineInfo.pSignalSemaphoreValues = pendingSubmit.signalSemaphoreValues.data();

		maxTimelineValue = std::max<uint64_t>(maxTimelineValue, pendingSubmit.timelineValue);

		myTimelineCallbacks.enqueue(std::make_tuple(std::move(pendingSubmit.callbacks), maxTimelineValue));
	}

	auto* submitBegin = reinterpret_cast<SubmitInfo<kVk>*>(timelinePtr);
	auto* submitPtr = submitBegin;
	timelinePtr = timelineBegin;

	for (const auto& pendingSubmit : myPendingSubmits)
	{
		auto& submitInfo = *(submitPtr++);

		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submitInfo.pNext = timelinePtr++;
		submitInfo.waitSemaphoreCount = pendingSubmit.waitSemaphores.size();
		submitInfo.pWaitSemaphores = pendingSubmit.waitSemaphores.data();
		submitInfo.pWaitDstStageMask = pendingSubmit.waitDstStageMasks.data();
		submitInfo.signalSemaphoreCount = pendingSubmit.signalSemaphores.size();
		submitInfo.pSignalSemaphores = pendingSubmit.signalSemaphores.data();
		submitInfo.commandBufferCount = pendingSubmit.commandBuffers.size();
		submitInfo.pCommandBuffers = pendingSubmit.commandBuffers.data();
	}

	QueueHostSyncInfo<kVk> syncInfo{{}, maxTimelineValue};
	{
		ZoneScopedN("Queue::submit::vkQueueSubmit");

		VK_CHECK(vkQueueSubmit(myQueue, myPendingSubmits.size(), submitBegin, VK_NULL_HANDLE));
	}

	myPendingSubmits.clear();

	return syncInfo;
}

template <>
void Queue<kVk>::WaitIdle() const
{
	ZoneScopedN("Queue::waitIdle");

	VK_CHECK(vkQueueWaitIdle(myQueue));
}

template <>
QueuePresentInfo<kVk> Queue<kVk>::Present()
{
	ZoneScopedN("Queue::present");

	Fence<kVk>::Wait(InternalGetDevice(), myPendingPresent.fences);

	PresentInfo<kVk> presentInfo{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
	presentInfo.waitSemaphoreCount = myPendingPresent.waitSemaphores.size();
	presentInfo.pWaitSemaphores = myPendingPresent.waitSemaphores.data();
	presentInfo.swapchainCount = myPendingPresent.swapchains.size();
	presentInfo.pSwapchains = myPendingPresent.swapchains.data();
	presentInfo.pImageIndices = myPendingPresent.imageIndices.data();
	presentInfo.pResults = myPendingPresent.results.data();

	CheckFlipOrPresentResult(vkQueuePresentKHR(myQueue, &presentInfo));

	return std::move(myPendingPresent);
}

template <>
QueueSubmitInfo<kVk> Queue<kVk>::InternalPrepareSubmit(QueueDeviceSyncInfo<kVk>&& syncInfo)
{
	ZoneScopedN("Queue::prepareSubmit");

	myPool.InternalEndCommands(VK_COMMAND_BUFFER_LEVEL_PRIMARY);

	auto& pendingCommands = myPool.InternalGetPendingCommands()[VK_COMMAND_BUFFER_LEVEL_PRIMARY];

	if (pendingCommands.empty())
		return {};

	QueueSubmitInfo<kVk> submitInfo{std::forward<QueueDeviceSyncInfo<kVk>>(syncInfo), {}, 0};
	submitInfo.commandBuffers.reserve(pendingCommands.size() * CommandBufferArray<kVk>::Capacity());

	for (const auto& [cmdArray, cmdTimelineValue] : pendingCommands)
	{
		ASSERT(!cmdArray.RecordingFlags());
		auto cmdCount = cmdArray.Head();
		std::copy_n(cmdArray.Data(), cmdCount, std::back_inserter(submitInfo.commandBuffers));
	}

	const auto [minSignalValue, maxSignalValue] = std::minmax_element(
		submitInfo.signalSemaphoreValues.begin(), submitInfo.signalSemaphoreValues.end());

	submitInfo.timelineValue = *maxSignalValue;

	myPool.InternalEnqueueSubmitted(std::move(pendingCommands), VK_COMMAND_BUFFER_LEVEL_PRIMARY, *maxSignalValue);
	
	return submitInfo;
}

template <>
void Queue<kVk>::Execute(uint8_t level, uint64_t timelineValue)
{
	ZoneScopedN("Queue::Execute");

	myPool.InternalEndCommands(level);

	auto& pendingCommands = myPool.InternalGetPendingCommands()[level];

	for (const auto& [cmdArray, cmdTimelineValue] : pendingCommands)
		vkCmdExecuteCommands(myPool.Commands(), cmdArray.Head(), cmdArray.Data());

	myPool.InternalEnqueueSubmitted(std::move(pendingCommands), level, timelineValue);
}
