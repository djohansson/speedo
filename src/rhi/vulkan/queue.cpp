#include "../queue.h"

#include "utils.h"

#include <tracy/TracyC.h>
#include <tracy/TracyVulkan.hpp>

namespace queue
{

struct UserData
{
	TracyVkCtx tracyContext{};
};

} // namespace queue

//

template <>
Queue<Vk>::Queue(
	const std::shared_ptr<Device<Vk>>& device,
	std::tuple<QueueCreateDesc<Vk>, QueueHandle<Vk>>&& descAndHandle)
	: DeviceObject(
		  device,
		  {"_Queue"},
		  1,
		  VK_OBJECT_TYPE_QUEUE,
		  reinterpret_cast<uint64_t*>(&std::get<1>(descAndHandle)))
	, myDesc(std::forward<QueueCreateDesc<Vk>>(std::get<0>(descAndHandle)))
	, myQueue(std::get<1>(descAndHandle))
{
// #if PROFILING_ENABLED
// 	{
// 		if (auto cmd =
// 				myDesc.tracingEnableInitCmd.value_or(CommandBufferHandle<Vk>{VK_NULL_HANDLE}))
// 		{
// 			myUserData = queue::UserData{tracy::CreateVkContext(
// 				device->getPhysicalDevice(),
// 				*device,
// 				myQueue,
// 				cmd,
// 				nullptr,
// 				nullptr)};
// 		}
// 	}
// #endif
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
	, myFence(std::exchange(other.myFence, {}))
	, myUserData(std::exchange(other.myUserData, {}))
{}

template <>
Queue<Vk>::~Queue()
{
// #if PROFILING_ENABLED
// 	{
// 		if (myDesc.tracingEnableInitCmd)
// 			tracy::DestroyVkContext(std::any_cast<queue::UserData>(&myUserData)->tracyContext);
// 	}
// #endif
}

template <>
Queue<Vk>& Queue<Vk>::operator=(Queue<Vk>&& other) noexcept
{
	DeviceObject::operator=(std::forward<Queue<Vk>>(other));
	myDesc = std::exchange(other.myDesc, {});
	myQueue = std::exchange(other.myQueue, {});
	myPendingSubmits = std::exchange(other.myPendingSubmits, {});
	myScratchMemory = std::exchange(other.myScratchMemory, {});
	myFence = std::exchange(other.myFence, {});
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
	std::swap(myFence, rhs.myFence);
	std::swap(myUserData, rhs.myUserData);
}

#if PROFILING_ENABLED
template <>
void Queue<Vk>::gpuScopeCollect(CommandBufferHandle<Vk> cmd)
{
	// {
	// 	if (myDesc.tracingEnableInitCmd)
	// 		TracyVkCollect(std::any_cast<queue::UserData>(&myUserData)->tracyContext, cmd);
	// }
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

	// if (myDesc.tracingEnableInitCmd)
	// {
	// 	return std::make_shared<tracy::VkCtxScope>(
	// 		std::any_cast<queue::UserData>(&myUserData)->tracyContext,
	// 		reinterpret_cast<const tracy::SourceLocationData*>(&srcLoc),
	// 		cmd,
	// 		true);
	// }

	return {};
}
#endif

template <>
uint64_t Queue<Vk>::submit()
{
	ZoneScopedN("Queue::submit");

	if (myPendingSubmits.empty())
		return 0;

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

	{
		ZoneScopedN("Queue::submit::vkQueueSubmit");

		VK_CHECK(vkQueueSubmit(myQueue, myPendingSubmits.size(), submitBegin, myFence));
	}

	myPendingSubmits.clear();

	myLastSubmitTimelineValue = maxTimelineValue;

	return maxTimelineValue;
}

template <>
void Queue<Vk>::waitIdle() const
{
	ZoneScopedN("Queue::waitIdle");

	VK_CHECK(vkQueueWaitIdle(myQueue));
}

template <>
void Queue<Vk>::present()
{
	ZoneScopedN("Queue::present");

	if (myPendingPresent.swapchains.empty())
		return;

	PresentInfo<Vk> presentInfo{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
	presentInfo.waitSemaphoreCount = myPendingPresent.waitSemaphores.size();
	presentInfo.pWaitSemaphores = myPendingPresent.waitSemaphores.data();
	presentInfo.swapchainCount = myPendingPresent.swapchains.size();
	presentInfo.pSwapchains = myPendingPresent.swapchains.data();
	presentInfo.pImageIndices = myPendingPresent.imageIndices.data();
	presentInfo.pResults = myPendingPresent.results.data();

	checkFlipOrPresentResult(vkQueuePresentKHR(myQueue, &presentInfo));

	myPendingPresent = {};
}

template <>
void QueueContext<Vk>::addCommandsFinishedCallback(std::function<void(uint64_t)>&& callback)
{
	myCommandsFinishedCallbacks.emplace_back(
		std::make_tuple(0, std::forward<std::function<void(uint64_t)>>(callback)));
}

template <>
void QueueContext<Vk>::internalFlushCommandsFinishedCallbacks(uint32_t timelineValue)
{
	while (!myCommandsFinishedCallbacks.empty())
	{
		auto& callback = myCommandsFinishedCallbacks.front();
		std::get<0>(callback) = timelineValue;
		getDevice()->addTimelineCallback(std::move(callback));
		myCommandsFinishedCallbacks.pop_front();
	}
}

template <>
QueueSubmitInfo<Vk> QueueContext<Vk>::prepareSubmit(QueueSyncInfo<Vk>&& syncInfo)
{
	ZoneScopedN("QueueContext::prepareSubmit");

	internalEndCommands(VK_COMMAND_BUFFER_LEVEL_PRIMARY);

	auto& pendingCommands = internalGetPendingCommands()[VK_COMMAND_BUFFER_LEVEL_PRIMARY];

	if (pendingCommands.empty())
		return {};

	QueueSubmitInfo<Vk> submitInfo{std::forward<QueueSyncInfo<Vk>>(syncInfo), {}, 0};
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
	internalFlushCommandsFinishedCallbacks(*maxSignalValue);

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
	internalFlushCommandsFinishedCallbacks(timelineValue);

	return timelineValue;
}

template <>
QueueContext<Vk>::QueueContext(
	const std::shared_ptr<Device<Vk>>& device,
	CommandPoolCreateDesc<Vk>&& commandPoolDesc,
	QueueCreateDesc<Vk>&& queueDesc)
	: CommandPool(device, std::forward<CommandPoolCreateDesc<Vk>>(commandPoolDesc))
	, myQueue(device, std::forward<QueueCreateDesc<Vk>>(queueDesc))
{}

template <>
QueueContext<Vk>::QueueContext(QueueContext&& other) noexcept
	: CommandPool(std::forward<QueueContext>(other))
	, myQueue(std::exchange(other.myQueue, {}))
	, myCommandsFinishedCallbacks(std::exchange(other.myCommandsFinishedCallbacks, {}))
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

			getDevice()->wait(cmdTimelineValue);
			getDevice()->processTimelineCallbacks(cmdTimelineValue);
		}
	}
}

template <>
QueueContext<Vk>& QueueContext<Vk>::operator=(QueueContext&& other) noexcept
{
	CommandPool::operator=(std::forward<QueueContext>(other));
	myCommandsFinishedCallbacks = std::exchange(other.myCommandsFinishedCallbacks, {});
	return *this;
}

template <>
void QueueContext<Vk>::swap(QueueContext& other) noexcept
{
	CommandPool::swap(other);
	std::swap(myCommandsFinishedCallbacks, other.myCommandsFinishedCallbacks);
}
