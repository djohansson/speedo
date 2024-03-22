#include "../queue.h"
#include "../fence.h"
#include "utils.h"

#include <tracy/TracyC.h>
#include <tracy/TracyVulkan.hpp>

namespace queue
{

struct UserData
{
#if PROFILING_ENABLED
	TracyVkCtx tracyContext{};
#endif
};

} // namespace queue

//

template <>
Queue<Vk>::Queue(
	const std::shared_ptr<Device<Vk>>& device,
	std::tuple<QueueCreateDesc<Vk>,
	QueueHandle<Vk>>&& descAndHandle)
	: DeviceObject(
		  device,
		  {"_Queue"},
		  1,
		  VK_OBJECT_TYPE_QUEUE,
		  reinterpret_cast<uint64_t*>(&std::get<1>(descAndHandle)))
	, myDesc(std::forward<QueueCreateDesc<Vk>>(std::get<0>(descAndHandle)))
	, myQueue(std::get<1>(descAndHandle))
{
#if PROFILING_ENABLED
	if (myDesc.tracingInitCmd)
		myUserData = queue::UserData{tracy::CreateVkContext(
			device->getPhysicalDevice(),
			*device,
			myQueue,
			myDesc.tracingInitCmd,
			nullptr,
			nullptr)};
#endif
}

template <>
Queue<Vk>::Queue(
	const std::shared_ptr<Device<Vk>>& device, QueueCreateDesc<Vk>&& desc)
	: Queue(
		  device,
		  std::make_tuple(
			  std::forward<QueueCreateDesc<Vk>>(desc),
			  [&device, &desc]()
			  {
				  QueueHandle<Vk> queue;
				  vkGetDeviceQueue(*device, desc.queueFamilyIndex, desc.queueIndex, &queue);
				  return queue;
			  }()))
{}

template <>
Queue<Vk>::Queue(Queue<Vk>&& other) noexcept
	: DeviceObject(std::forward<Queue<Vk>>(other))
	, myDesc(std::exchange(other.myDesc, {}))
	, myQueue(std::exchange(other.myQueue, {}))
	, myPendingSubmits(std::exchange(other.myPendingSubmits, {}))
	, myScratchMemory(std::exchange(other.myScratchMemory, {}))
	, myUserData(std::exchange(other.myUserData, {}))
{}

template <>
Queue<Vk>::~Queue()
{
#if PROFILING_ENABLED
	if (myDesc.tracingInitCmd)
		tracy::DestroyVkContext(std::any_cast<queue::UserData>(&myUserData)->tracyContext);
#endif
}

template <>
Queue<Vk>& Queue<Vk>::operator=(Queue<Vk>&& other) noexcept
{
	DeviceObject::operator=(std::forward<Queue<Vk>>(other));
	myDesc = std::exchange(other.myDesc, {});
	myQueue = std::exchange(other.myQueue, {});
	myPendingSubmits = std::exchange(other.myPendingSubmits, {});
	myScratchMemory = std::exchange(other.myScratchMemory, {});
	myUserData = std::exchange(other.myUserData, {});
	return *this;
}

template <>
void Queue<Vk>::swap(Queue& rhs) noexcept
{
	DeviceObject::swap(rhs);
	std::swap(myDesc, rhs.myDesc);
	std::swap(myQueue, rhs.myQueue);
	std::swap(myPendingSubmits, rhs.myPendingSubmits);
	std::swap(myScratchMemory, rhs.myScratchMemory);
	std::swap(myUserData, rhs.myUserData);
}

#if PROFILING_ENABLED
template <>
void Queue<Vk>::gpuScopeCollect(CommandBufferHandle<Vk> cmd)
{
	if (myDesc.tracingInitCmd)
		TracyVkCollect(std::any_cast<queue::UserData>(&myUserData)->tracyContext, cmd);
}

template <>
std::shared_ptr<void>
Queue<Vk>::internalGpuScope(CommandBufferHandle<Vk> cmd, const SourceLocationData& srcLoc)
{
	static_assert(sizeof(SourceLocationData) == sizeof(tracy::SourceLocationData));
	static_assert(offsetof(SourceLocationData, name) == offsetof(tracy::SourceLocationData, name));
	static_assert(offsetof(SourceLocationData, function) == offsetof(tracy::SourceLocationData, function));
	static_assert(offsetof(SourceLocationData, file) == offsetof(tracy::SourceLocationData, file));
	static_assert(offsetof(SourceLocationData, line) == offsetof(tracy::SourceLocationData, line));
	static_assert(offsetof(SourceLocationData, color) == offsetof(tracy::SourceLocationData, color));

	if (myDesc.tracingInitCmd)
	{
		return std::make_shared<tracy::VkCtxScope>(
			std::any_cast<queue::UserData>(&myUserData)->tracyContext,
			reinterpret_cast<const tracy::SourceLocationData*>(&srcLoc),
			cmd,
			true);
	}

	return {};
}
#endif

template <>
QueueHostSyncInfo<Vk> Queue<Vk>::submit()
{
	ZoneScopedN("Queue::submit");

	if (myPendingSubmits.empty())
		return {};

	myScratchMemory.resize(
		(sizeof(SubmitInfo<Vk>) + sizeof(TimelineSemaphoreSubmitInfo<Vk>)) *
		myPendingSubmits.size());

	auto timelineBegin = reinterpret_cast<TimelineSemaphoreSubmitInfo<Vk>*>(myScratchMemory.data());
	auto timelinePtr = timelineBegin;

	uint64_t maxTimelineValue = 0ull;

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
	}

	auto submitBegin = reinterpret_cast<SubmitInfo<Vk>*>(timelinePtr);
	auto submitPtr = submitBegin;
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

	QueueHostSyncInfo<Vk> syncInfo{{}, maxTimelineValue};
	{
		ZoneScopedN("Queue::submit::vkQueueSubmit");

		VK_CHECK(vkQueueSubmit(myQueue, myPendingSubmits.size(), submitBegin, VK_NULL_HANDLE));
	}

	myPendingSubmits.clear();

	return syncInfo;
}

template <>
void Queue<Vk>::waitIdle() const
{
	ZoneScopedN("Queue::waitIdle");

	VK_CHECK(vkQueueWaitIdle(myQueue));
}

template <>
QueuePresentInfo<Vk> Queue<Vk>::present()
{
	ZoneScopedN("Queue::present");

	Fence<Vk>::wait(getDevice(), myPendingPresent.fences);

	PresentInfo<Vk> presentInfo{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
	presentInfo.waitSemaphoreCount = myPendingPresent.waitSemaphores.size();
	presentInfo.pWaitSemaphores = myPendingPresent.waitSemaphores.data();
	presentInfo.swapchainCount = myPendingPresent.swapchains.size();
	presentInfo.pSwapchains = myPendingPresent.swapchains.data();
	presentInfo.pImageIndices = myPendingPresent.imageIndices.data();
	presentInfo.pResults = myPendingPresent.results.data();

	checkFlipOrPresentResult(vkQueuePresentKHR(myQueue, &presentInfo));

	return std::move(myPendingPresent);
}

template <>
void QueueContext<Vk>::addTimelineCallback(TimelineCallback&& callback)
{
	ZoneScopedN("QueueContext::addTimelineCallback");

	myTimelineCallbacks.emplace_back(std::forward<TimelineCallback>(callback));
}

template <>
bool QueueContext<Vk>::processTimelineCallbacks(uint64_t timelineValue)
{
	ZoneScopedN("QueueContext::processTimelineCallbacks");

	while (!myTimelineCallbacks.empty())
	{
		const auto& [commandBufferTimelineValue, callback] = myTimelineCallbacks.front();

		if (commandBufferTimelineValue > timelineValue)
			return false;

		callback(commandBufferTimelineValue);

		myTimelineCallbacks.pop_front();
	}

	return true;
}

template <>
QueueSubmitInfo<Vk> QueueContext<Vk>::prepareSubmit(QueueDeviceSyncInfo<Vk>&& syncInfo)
{
	ZoneScopedN("QueueContext::prepareSubmit");

	internalEndCommands(VK_COMMAND_BUFFER_LEVEL_PRIMARY);

	auto& pendingCommands = internalGetPendingCommands()[VK_COMMAND_BUFFER_LEVEL_PRIMARY];

	if (pendingCommands.empty())
		return {};

	QueueSubmitInfo<Vk> submitInfo{std::forward<QueueDeviceSyncInfo<Vk>>(syncInfo), {}, 0};
	submitInfo.commandBuffers.reserve(pendingCommands.size() * CommandBufferArray<Vk>::capacity());

	for (const auto& [cmdArray, cmdTimelineValue] : pendingCommands)
	{
		assert(!cmdArray.recordingFlags());
		auto cmdCount = cmdArray.head();
		std::copy_n(cmdArray.data(), cmdCount, std::back_inserter(submitInfo.commandBuffers));
	}

	const auto [minSignalValue, maxSignalValue] = std::minmax_element(
		submitInfo.signalSemaphoreValues.begin(), submitInfo.signalSemaphoreValues.end());

	submitInfo.timelineValue = *maxSignalValue;

	internalEnqueueSubmitted(std::move(pendingCommands), VK_COMMAND_BUFFER_LEVEL_PRIMARY, *maxSignalValue);
	
	return submitInfo;
}

template <>
uint64_t QueueContext<Vk>::execute(uint32_t index)
{
	ZoneScopedN("QueueContext::execute");

	internalEndCommands(index);

	auto& pendingCommands = internalGetPendingCommands()[index];

	for (const auto& [cmdArray, cmdTimelineValue] : pendingCommands)
		vkCmdExecuteCommands(commands(), cmdArray.head(), cmdArray.data());

	auto timelineValue = getDevice()->timelineValue().load(std::memory_order_relaxed);

	internalEnqueueSubmitted(std::move(pendingCommands), index, timelineValue);

	return timelineValue;
}

template <>
QueueContext<Vk>::QueueContext(
	const std::shared_ptr<Device<Vk>>& device,
	CommandPoolCreateDesc<Vk>&& commandPoolDesc,
	QueueCreateDesc<Vk>&& queueDesc)
	: CommandPool(device, std::forward<CommandPoolCreateDesc<Vk>>(commandPoolDesc))
	, myQueue(
		device,
#if PROFILING_ENABLED
		[this, &device, &queueDesc]
		{
			if (device->getPhysicalDeviceInfo().queueFamilyProperties[queueDesc.queueFamilyIndex].queueFlags & VK_QUEUE_GRAPHICS_BIT)
				queueDesc.tracingInitCmd = commands(CommandBufferAccessScopeDesc<Vk>(false));

			return queueDesc;
		}())
#else
		std::forward<QueueCreateDesc<Vk>>(queueDesc))
#endif
{
	static_assert((uint32_t)QueueFamilyFlagBits_Graphics == (uint32_t)VK_QUEUE_GRAPHICS_BIT);
	static_assert((uint32_t)QueueFamilyFlagBits_Compute == (uint32_t)VK_QUEUE_COMPUTE_BIT);
	static_assert((uint32_t)QueueFamilyFlagBits_Transfer == (uint32_t)VK_QUEUE_TRANSFER_BIT);
	static_assert((uint32_t)QueueFamilyFlagBits_SparseBinding == (uint32_t)VK_QUEUE_SPARSE_BINDING_BIT);
	static_assert((uint32_t)QueueFamilyFlagBits_VideoDecode == (uint32_t)VK_QUEUE_VIDEO_DECODE_BIT_KHR);
	static_assert((uint32_t)QueueFamilyFlagBits_VideoEncode == (uint32_t)VK_QUEUE_VIDEO_ENCODE_BIT_KHR);
}

template <>
QueueContext<Vk>::QueueContext(QueueContext&& other) noexcept
	: CommandPool(std::forward<QueueContext>(other))
	, myQueue(std::exchange(other.myQueue, {}))
{}

template <>
QueueContext<Vk>::~QueueContext()
{
	ZoneScopedN("~QueueContext()");

	for (auto& submittedCommandList : internalGetSubmittedCommands())
	{
		if (!submittedCommandList.empty())
		{
			const auto& [cmdArray, cmdTimelineValue] = submittedCommandList.back();

			processTimelineCallbacks(cmdTimelineValue);
		}
	}

	assert(myTimelineCallbacks.empty());
}

template <>
QueueContext<Vk>& QueueContext<Vk>::operator=(QueueContext&& other) noexcept
{
	CommandPool::operator=(std::forward<QueueContext>(other));
	myQueue = std::exchange(other.myQueue, {});
	myTimelineCallbacks = std::exchange(other.myTimelineCallbacks, {});
	return *this;
}

template <>
void QueueContext<Vk>::swap(QueueContext& other) noexcept
{
	CommandPool::swap(other);
	std::swap(myQueue, other.myQueue);
	std::swap(myTimelineCallbacks, other.myTimelineCallbacks);
}
