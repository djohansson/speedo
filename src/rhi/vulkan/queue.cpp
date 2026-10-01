#include <rhi/queue.h>
#include <rhi/device.h>
#include <rhi/instance.h>
#include <rhi/rhiapplication.h>
#include <rhi/vulkan/utils.h>

#include <tracy/TracyC.h>
#include <tracy/TracyVulkan.hpp>

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
					GetDesc().queueFamilyIndex,
					GetDesc().levelCount,
					GetDesc().supportsProfiling
				}
			),
			CommandPool<kVk>(
				CommandPoolCreateDesc<kVk>
				{
					SuperType::CreateDeviceObjectCreateDesc("CommandPool 1"),
					VK_COMMAND_POOL_CREATE_TRANSIENT_BIT|VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
					GetDesc().queueFamilyIndex,
					GetDesc().levelCount,
					GetDesc().supportsProfiling
				}
			)
		}
	)
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
	if (GetDesc().supportsProfiling)
		myProfilingContext = CreateVkContext(
			GetDevice().GetPhysicalDevice(),
			GetDevice(),
			myQueue,
			GetPool().Commands(CommandBufferAccessScopeDesc<kVk>(false)),
			nullptr,
			nullptr);
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
	std::swap(myProfilingContext, other.myProfilingContext);
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
	Swap(other);
	return *this;
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
QueueHostSyncInfo<kVk> Queue<kVk>::Present(Result<kVk>* presentResult)
{
	ZoneScopedN("Queue::Present");

	static bool gSupportsPresentFence = GetDevice().SupportsFeature(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR, GetInstance());
	static bool gSupportsPresentId = SupportsExtension(VK_KHR_PRESENT_ID_EXTENSION_NAME, GetInstance());

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
			*presentResult = vkResult;
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

	const auto [minSignalValue, maxSignalValue] = std::ranges::minmax_element(
		submitInfo.signalSemaphoreValues);

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

} // namespace rhi
