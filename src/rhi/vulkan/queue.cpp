#include "../queue.h"
#include "utils.h"

#include <tracy/TracyC.h>
#include <tracy/TracyVulkan.hpp>

template <>
bool Queue<kVk>::SubmitCallbacks(TaskExecutor& executor, uint64_t timelineValue) const
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
	const CommandPoolCreateDesc<kVk>& commandPoolDesc,
	std::tuple<QueueCreateDesc<kVk>, QueueHandle<kVk>>&& descAndHandle)
	: DeviceObject(
		  device,
		  {"_Queue"},
		  1,
		  VK_OBJECT_TYPE_QUEUE,
		  reinterpret_cast<uint64_t*>(&std::get<1>(descAndHandle)),
		  uuids::uuid_system_generator{}())
	, myDesc(std::forward<QueueCreateDesc<kVk>>(std::get<0>(descAndHandle)))
	, myQueue(std::get<1>(descAndHandle))
	, myPools({CommandPool<kVk>(device, CommandPoolCreateDesc<kVk>{commandPoolDesc}),
			   CommandPool<kVk>(device, CommandPoolCreateDesc<kVk>{commandPoolDesc})})
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

#if (SPEEDO_PROFILING_LEVEL > 0)
	if (commandPoolDesc.supportsProfiling)
		myProfilingContext = CreateVkContext(
			device->GetPhysicalDevice(),
			*device,
			myQueue,
			GetPool().Commands(CommandBufferAccessScopeDesc<kVk>(false)),
			nullptr,
			nullptr);
#endif
}

template <>
Queue<kVk>::Queue(
	const std::shared_ptr<Device<kVk>>& device,
	const CommandPoolCreateDesc<kVk>& commandPoolDesc,
	QueueCreateDesc<kVk>&& queueDesc)
	: Queue(
		device,
		commandPoolDesc,
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
	, myPools(std::exchange(other.myPools, {}))
	, myPendingSubmits(std::exchange(other.myPendingSubmits, {}))
	, myScratchMemory(std::exchange(other.myScratchMemory, {}))
{
#if (SPEEDO_PROFILING_LEVEL > 0)
	std::swap(myProfilingContext, other.myProfilingContext);
#endif
}

template <>
Queue<kVk>::~Queue()
{
	using namespace tracy;

	ZoneScopedN("Queue::~Queue()");

#if (SPEEDO_PROFILING_LEVEL > 0)
	if (myProfilingContext != nullptr)
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
	myPools = std::exchange(other.myPools, {});
	myPendingSubmits = std::exchange(other.myPendingSubmits, {});
	myScratchMemory = std::exchange(other.myScratchMemory, {});
	std::swap(myTimelineCallbacks, other.myTimelineCallbacks);
	decltype(other.myTimelineCallbacks) tmp;
	std::swap(other.myTimelineCallbacks, tmp);
#if (SPEEDO_PROFILING_LEVEL > 0)
	std::swap(myProfilingContext, other.myProfilingContext);
#endif
	return *this;
}

template <>
void Queue<kVk>::Swap(Queue& other) noexcept
{
	DeviceObject::Swap(other);
	std::swap(myDesc, other.myDesc);
	std::swap(myQueue, other.myQueue);
	std::swap(myPools, other.myPools);
	std::swap(myPendingSubmits, other.myPendingSubmits);
	std::swap(myScratchMemory, other.myScratchMemory);
	std::swap(myTimelineCallbacks, other.myTimelineCallbacks);
#if (SPEEDO_PROFILING_LEVEL > 0)
	std::swap(myProfilingContext, other.myProfilingContext);
#endif
}

template <>
void Queue<kVk>::CollectGpuScope(CommandBufferHandle<kVk> cmd)
{
#if (SPEEDO_PROFILING_LEVEL > 0)
	if (myProfilingContext != nullptr)
		TracyVkCollect(static_cast<TracyVkCtx>(myProfilingContext), cmd);
#endif
}

template <>
std::shared_ptr<void>
Queue<kVk>::InternalGpuScope(CommandBufferHandle<kVk> cmd, const SourceLocationData& srcLoc)
{
	if (gVkCmdSetCheckpointNV != nullptr)
		gVkCmdSetCheckpointNV(cmd, srcLoc.name);

#if (SPEEDO_PROFILING_LEVEL > 0)
	static_assert(sizeof(SourceLocationData) == sizeof(tracy::SourceLocationData));
	static_assert(offsetof(SourceLocationData, name) == offsetof(tracy::SourceLocationData, name));
	static_assert(offsetof(SourceLocationData, function) == offsetof(tracy::SourceLocationData, function));
	static_assert(offsetof(SourceLocationData, file) == offsetof(tracy::SourceLocationData, file));
	static_assert(offsetof(SourceLocationData, line) == offsetof(tracy::SourceLocationData, line));
	static_assert(offsetof(SourceLocationData, color) == offsetof(tracy::SourceLocationData, color));
	if (myProfilingContext != nullptr)
	{
		return std::make_shared<tracy::VkCtxScope>(
			static_cast<TracyVkCtx>(myProfilingContext),
			reinterpret_cast<const tracy::SourceLocationData*>(&srcLoc),
			cmd,
			true);
	}
#endif

	return {};
}

template <>
QueueHostSyncInfo<kVk> Queue<kVk>::Submit()
{
	ZoneScopedN("Queue::Submit");

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

		myTimelineCallbacks.enqueue(std::make_tuple(pendingSubmit.callbacks, maxTimelineValue));
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

	QueueHostSyncInfo<kVk> result;
	result.fences.emplace_back(InternalGetDevice(), FenceCreateDesc<kVk>{.name = "submitFence"});
	result.maxTimelineValue = maxTimelineValue;
	
	Result<kVk> submitResult;
	{
		ZoneScopedN("Queue::Submit::vkQueueSubmit");

		VK_CHECK(vkQueueSubmit(myQueue, myPendingSubmits.size(), submitBegin, result.fences.back()), reinterpret_cast<uintptr_t>(myQueue));
	}

	myPendingSubmits.clear();

	return result;
}

template <>
void Queue<kVk>::WaitIdle() const
{
	ZoneScopedN("Queue::WaitIdle");

	VK_CHECK(vkQueueWaitIdle(myQueue), reinterpret_cast<uintptr_t>(myQueue));
}

template <>
QueueHostSyncInfo<kVk> Queue<kVk>::Present()
{
	ZoneScopedN("Queue::Present");

	bool supportsPresentFence = InternalGetDevice()->SupportsFeature(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR);
	bool supportsPresentId = SupportsExtension(VK_KHR_PRESENT_ID_EXTENSION_NAME, *InternalGetDevice()->GetInstance());

	static uint64_t gPresentId = 0ULL;
	std::vector<uint64_t> presentIds;
	if (supportsPresentId)
	{
		presentIds.resize(myPendingPresent.swapchains.size());
		for (size_t i = 0; i < myPendingPresent.swapchains.size(); ++i)
			presentIds[i] = gPresentId++;
	}

	QueueHostSyncInfo<kVk> result;
	if (supportsPresentFence)
		result.fences.emplace_back(InternalGetDevice(), FenceCreateDesc<kVk>{.name = "presentFence"});
	if (supportsPresentId)
		result.presentIds = std::move(presentIds);

	PresentFenceInfo<kVk> presentFenceInfo{VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT};
	ENSURE(myPendingPresent.swapchains.size() == 1); // todo: support multiple swapchains, implement Fence arrays
	presentFenceInfo.swapchainCount = myPendingPresent.swapchains.size();
	presentFenceInfo.pFences = supportsPresentFence ? &result.fences.front().GetHandle() : nullptr;

	void* presentFenceInfoPtr = 
		supportsPresentFence ?
			reinterpret_cast<void*>(&presentFenceInfo) :
			nullptr;

	PresentId<kVk> presentId{VK_STRUCTURE_TYPE_PRESENT_ID_KHR};
	presentId.pNext = presentFenceInfoPtr;
	presentId.swapchainCount = myPendingPresent.swapchains.size();
	presentId.pPresentIds = supportsPresentId ? result.presentIds.data() : nullptr;

	void* presentIdPtr = 
		supportsPresentId ?
			reinterpret_cast<void*>(&presentId) :
			nullptr;

	PresentInfo<kVk> presentInfo{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
	presentInfo.pNext = presentIdPtr ? presentIdPtr : presentFenceInfoPtr;
	presentInfo.waitSemaphoreCount = myPendingPresent.waitSemaphores.size();
	presentInfo.pWaitSemaphores = myPendingPresent.waitSemaphores.data();
	presentInfo.swapchainCount = myPendingPresent.swapchains.size();
	presentInfo.pSwapchains = myPendingPresent.swapchains.data();
	presentInfo.pImageIndices = myPendingPresent.imageIndices.data();
	presentInfo.pResults = myPendingPresent.results.data();

	{
		ZoneScopedN("Queue::Present::vkQueuePresentKHR");
		VK_CHECK(vkQueuePresentKHR(myQueue, &presentInfo), reinterpret_cast<uintptr_t>(myQueue));
	}

	myPendingPresent = {};

	return result;
}

template <>
QueueSubmitInfo<kVk> Queue<kVk>::InternalPrepareSubmit(QueueDeviceSyncInfo<kVk>&& syncInfo)
{
	ZoneScopedN("Queue::PrepareSubmit");

	GetPool().InternalEndCommands(VK_COMMAND_BUFFER_LEVEL_PRIMARY);

	auto& pendingCommands = GetPool().InternalGetPendingCommands()[VK_COMMAND_BUFFER_LEVEL_PRIMARY];

	if (pendingCommands.empty())
		return {};

	QueueSubmitInfo<kVk> submitInfo{std::forward<QueueDeviceSyncInfo<kVk>>(syncInfo), {}, 0};
	submitInfo.commandBuffers.reserve(pendingCommands.size() * CommandBufferArray<kVk>::Capacity());

	for (const auto& [cmdArray, cmdTimelineValue] : pendingCommands)
	{
		ENSURE(!cmdArray.RecordingFlags());
		auto cmdCount = cmdArray.Head();
		std::copy_n(cmdArray.Data(), cmdCount, std::back_inserter(submitInfo.commandBuffers));
	}

	const auto [minSignalValue, maxSignalValue] = std::minmax_element(
		submitInfo.signalSemaphoreValues.begin(), submitInfo.signalSemaphoreValues.end());

	submitInfo.timelineValue = *maxSignalValue;

	GetPool().InternalEnqueueSubmitted(std::move(pendingCommands), VK_COMMAND_BUFFER_LEVEL_PRIMARY, *maxSignalValue);
	
	return submitInfo;
}

template <>
void Queue<kVk>::Execute(uint8_t level, uint64_t timelineValue)
{
	ZoneScopedN("Queue::Execute");

	GetPool().InternalEndCommands(level);

	auto& pendingCommands = GetPool().InternalGetPendingCommands()[level];

	for (const auto& [cmdArray, cmdTimelineValue] : pendingCommands)
		vkCmdExecuteCommands(GetPool().Commands(), cmdArray.Head(), cmdArray.Data());

	GetPool().InternalEnqueueSubmitted(std::move(pendingCommands), level, timelineValue);
}
