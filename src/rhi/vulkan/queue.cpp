#include <rhi/queue.h>
#include <rhi/device.h>
#include <rhi/instance.h>
#include <rhi/rhi.h>
#include <rhi/vulkan/utils.h>


namespace rhi
{

IMPLEMENT_OBJECT_GETINSTANCE(Queue<kVk>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(Queue<kVk>);

template <>
bool Queue<kVk>::SubmitCallbacks(core::TaskExecutor& executor, uint64_t timelineValue) const
{
	ZoneScopedN("Queue::SubmitCallbacks");

	TimelineCallbackData callbackData;
	std::vector<TimelineCallbackData> waitingCallbacks;
	while (myTimelineCallbacks.try_dequeue(callbackData))
	{
		auto& [callbackVector, callbackTimelineValue] = callbackData;

		if (timelineValue <= callbackTimelineValue)
			waitingCallbacks.emplace_back(std::move(callbackData));
		else
			executor.Submit(std::span(callbackVector.data(), callbackVector.size()));
	}

	myTimelineCallbacks.enqueue_bulk(waitingCallbacks.data(), waitingCallbacks.size());

	return true;
}

template <>
Queue<kVk>::Queue(
	CreateDescType&& desc,
	QueueHandle<kVk>&& handle)
	: DeviceObject<Queue<kVk>>(std::forward<CreateDescType>(desc))
	, myQueue(std::forward<QueueHandle<kVk>>(handle))
	, myPools(
		{
			CommandPool<kVk>(
				CommandPoolCreateDesc<kVk>
				{
					SuperType::CreateDeviceObjectCreateDesc("CommandPool 0"),
					VK_COMMAND_POOL_CREATE_TRANSIENT_BIT|VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
					GetDesc().queueFamilyIndex
				}
			),
			CommandPool<kVk>(
				CommandPoolCreateDesc<kVk>
				{
					SuperType::CreateDeviceObjectCreateDesc("CommandPool 1"),
					VK_COMMAND_POOL_CREATE_TRANSIENT_BIT|VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
					GetDesc().queueFamilyIndex
				}
			)
		}
	)
{
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
	// a ring of 4096 timestamps (a frame writes about 40): KosmicKrisp's query pools are Metal counter heaps, of at most
	// 4096 samples
	if (GetDesc().supportsProfiling)
		myProfiler = std::make_unique<GpuProfiler<kVk>>(
			QueryPool<kVk>(QueryPoolCreateDesc<kVk>{
				SuperType::CreateDeviceObjectCreateDesc(std::format("{} Profiler", GetDesc().name)), 4096}),
			myQueue,
			GetPool().Allocate(),
			GetDesc().name);
#endif
}

template <>
Queue<kVk>::Queue(QueueCreateDesc<kVk>&& queueDesc)
	: Queue(
		std::forward<QueueCreateDesc<kVk>>(queueDesc),
		// read from queueDesc, not GetDesc(): this runs before the delegated constructor has initialized the base
		[&queueDesc]
		{
			QueueHandle<kVk> queue;
			vkGetDeviceQueue(
				queueDesc.device,
				queueDesc.queueFamilyIndex,
				queueDesc.queueIndex,
				&queue);
			Track(queueDesc.device, VK_OBJECT_TYPE_QUEUE, queue, GetDebugName(queueDesc));
			return queue;
		}())
{}

template <>
void Queue<kVk>::Swap(Queue& other) noexcept
{
	DeviceObject<Queue<kVk>>::Swap(other);
	std::swap(myQueue, other.myQueue);
	std::swap(myPools, other.myPools);
	std::swap(myPendingSubmits, other.myPendingSubmits);
	std::swap(myScratchMemory, other.myScratchMemory);
	std::swap(myTimelineCallbacks, other.myTimelineCallbacks);
#if (SPEEDO_PROFILING_LEVEL > 0)
	std::swap(myProfiler, other.myProfiler);
#endif
}

template <>
Queue<kVk>::Queue(Queue<kVk>&& other) noexcept
{
	Swap(other);
}

template <>
Queue<kVk>::~Queue()
{
	ZoneScopedN("Queue::~Queue()");

	myProfiler.reset();

	ASSERT(myTimelineCallbacks.size_approx() == 0);

	// queues are owned by the device, and only stop being tracked
	if (IsValid())
		Untrack(VK_OBJECT_TYPE_QUEUE, myQueue);
}

template <>
Queue<kVk>& Queue<kVk>::operator=(Queue<kVk>&& other) noexcept
{
	Swap(other);
	return *this;
}

template <>
GpuProfiler<kVk>::Scope Queue<kVk>::InternalGpuScope(CommandBufferHandle<kVk> cmd, const SourceLocationData& srcLoc)
{
	if (gVkCmdSetCheckpointNV != nullptr)
		gVkCmdSetCheckpointNV(cmd, srcLoc.name);

	if (myProfiler)
		return myProfiler->Begin(cmd, srcLoc);

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

	// the wait stages in vulkan's terms, alive until the submit
	std::vector<std::vector<VkPipelineStageFlags>> waitDstStageMasks;
	waitDstStageMasks.reserve(myPendingSubmits.size());

	for (const auto& pendingSubmit : myPendingSubmits)
	{
		auto& submitInfo = *(submitPtr++);

		auto& stageMasks = waitDstStageMasks.emplace_back();
		stageMasks.reserve(pendingSubmit.waitDstStageMasks.size());
		for (auto stages : pendingSubmit.waitDstStageMasks)
			stageMasks.push_back(vk::ToVk(stages));

		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submitInfo.pNext = timelinePtr++;
		submitInfo.waitSemaphoreCount = pendingSubmit.waitSemaphores.size();
		submitInfo.pWaitSemaphores = pendingSubmit.waitSemaphores.data();
		submitInfo.pWaitDstStageMask = stageMasks.data();
		submitInfo.signalSemaphoreCount = pendingSubmit.signalSemaphores.size();
		submitInfo.pSignalSemaphores = pendingSubmit.signalSemaphores.data();
		submitInfo.commandBufferCount = pendingSubmit.commandBuffers.size();
		submitInfo.pCommandBuffers = pendingSubmit.commandBuffers.data();
	}

	QueueHostSyncInfo<kVk> result;
	result.fences.emplace_back(FenceCreateDesc<kVk>{SuperType::CreateDeviceObjectCreateDesc("submitFence")});
	result.maxTimelineValue = maxTimelineValue;
	
	Result<kVk> submitResult;
	{
		ZoneScopedN("Queue::Submit::vkQueueSubmit");

		VK_CHECK(vkQueueSubmit(
			myQueue,
			myPendingSubmits.size(),
			submitBegin,
			result.fences.back()),
			reinterpret_cast<uintptr_t>(myQueue));
	}

	gSubmitCount.fetch_add(1, std::memory_order_relaxed);
	gSubmitBatchCount.fetch_add(myPendingSubmits.size(), std::memory_order_relaxed);

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
QueueHostSyncInfo<kVk> Queue<kVk>::Present(PresentResult* presentResult)
{
	ZoneScopedN("Queue::Present");

	static bool gSupportsPresentFence = GetDevice().SupportsFeature(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR, GetInstance());
	static bool gSupportsPresentId = SupportsExtension(VK_KHR_PRESENT_ID_EXTENSION_NAME, GetDevice().GetPhysicalDevice());

	const auto swapchainCount = static_cast<uint32_t>(myPendingPresent.swapchains.size());
	ENSURE(swapchainCount > 0);
	ENSURE(myPendingPresent.imageIndices.size() == swapchainCount);
	ENSURE(!gSupportsPresentId || myPendingPresent.presentIds.size() == swapchainCount);

	// one present fence per swapchain (VkSwapchainPresentFenceInfoEXT takes an array matching pSwapchains)
	QueueHostSyncInfo<kVk> result;
	std::vector<FenceHandle<kVk>> presentFences;
	if (gSupportsPresentFence)
	{
		result.fences.reserve(swapchainCount);
		presentFences.reserve(swapchainCount);
		for (uint32_t swapchainIt = 0; swapchainIt < swapchainCount; swapchainIt++)
		{
			auto& fence = result.fences.emplace_back(FenceCreateDesc<kVk>{SuperType::CreateDeviceObjectCreateDesc("presentFence")});
			presentFences.push_back(fence.GetHandle());
		}
	}

	PresentFenceInfo<kVk> presentFenceInfo{.sType=VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT};
	presentFenceInfo.swapchainCount = swapchainCount;
	presentFenceInfo.pFences = presentFences.data();

	void* presentFenceInfoPtr = 
		gSupportsPresentFence ?
			reinterpret_cast<void*>(&presentFenceInfo) :
			nullptr;

	PresentId<kVk> presentId{.sType=VK_STRUCTURE_TYPE_PRESENT_ID_KHR};
	presentId.pNext = presentFenceInfoPtr;
	presentId.swapchainCount = myPendingPresent.swapchains.size();
	presentId.pPresentIds = gSupportsPresentId ? myPendingPresent.presentIds.data() : nullptr;

	void* presentIdPtr = 
		gSupportsPresentId ?
			reinterpret_cast<void*>(&presentId) :
			nullptr;

	PresentInfo<kVk> presentInfo{.sType=VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
	presentInfo.pNext = (presentIdPtr != nullptr) ? presentIdPtr : presentFenceInfoPtr;
	presentInfo.waitSemaphoreCount = myPendingPresent.waitSemaphores.size();
	presentInfo.pWaitSemaphores = myPendingPresent.waitSemaphores.data();
	presentInfo.swapchainCount = myPendingPresent.swapchains.size();
	presentInfo.pSwapchains = myPendingPresent.swapchains.data();
	presentInfo.pImageIndices = myPendingPresent.imageIndices.data();
	presentInfo.pResults = myPendingPresent.results.data();

	{
		ZoneScopedN("Queue::Present::vkQueuePresentKHR");
		// out of date / suboptimal are expected while resizing: reported through result, so the swapchain can be recreated
		auto vkResult = vkQueuePresentKHR(myQueue, &presentInfo);
		if (vkResult != VK_SUBOPTIMAL_KHR && vkResult != VK_ERROR_OUT_OF_DATE_KHR)
			VK_CHECK(vkResult, reinterpret_cast<uintptr_t>(myQueue));
		if (presentResult != nullptr)
			*presentResult = vk::ToPresentResult(vkResult);
	}

	myPendingPresent = {};

	return result;
}

template <>
void Queue<kVk>::EnqueueSubmit(QueueDeviceSyncInfo<kVk>&& syncInfo, std::vector<CommandBuffer<kVk>>&& commands)
{
	ZoneScopedN("Queue::EnqueueSubmit");

	QueueSubmitInfo<kVk> submitInfo{std::move(syncInfo), {}, 0};
	submitInfo.commandBuffers.reserve(commands.size());
	for (auto& command : commands)
	{
		ENSURE(!command.IsSecondary());
		submitInfo.commandBuffers.push_back(command.Release());
	}
	if (!submitInfo.signalSemaphoreValues.empty())
		submitInfo.timelineValue = *std::ranges::max_element(submitInfo.signalSemaphoreValues);

	myPendingSubmits.emplace_back(std::move(submitInfo));
}

template <>
void Queue<kVk>::EnqueueSubmit(QueueDeviceSyncInfo<kVk>&& syncInfo, CommandBuffer<kVk>&& commands)
{
	std::vector<CommandBuffer<kVk>> list;
	list.push_back(std::move(commands));
	EnqueueSubmit(std::move(syncInfo), std::move(list));
}

template <>
void Queue<kVk>::EnqueueSubmit(QueueDeviceSyncInfo<kVk>&& syncInfo)
{
	EnqueueSubmit(std::move(syncInfo), std::vector<CommandBuffer<kVk>>{});
}

} // namespace rhi
