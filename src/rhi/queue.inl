namespace rhi
{

template <GraphicsApi G>
QueueHostSyncInfo<G>& QueueHostSyncInfo<G>::operator|=(QueueHostSyncInfo<G>&& other)
{
	fences.insert(
		fences.end(),
		std::make_move_iterator(other.fences.begin()),
		std::make_move_iterator(other.fences.end()));
	maxTimelineValue = std::max(maxTimelineValue, other.maxTimelineValue);
	
	return *this;
}

template <GraphicsApi G>
QueueHostSyncInfo<G> operator|(QueueHostSyncInfo<G>&& lhs, QueueHostSyncInfo<G>&& rhs)
{
	return std::forward<QueueHostSyncInfo<G>>(lhs |= std::forward<QueueHostSyncInfo<G>>(rhs));
}

template <GraphicsApi G>
QueuePresentInfo<G>& QueuePresentInfo<G>::operator|=(QueuePresentInfo<G>&& other)
{
	waitSemaphores.insert(
		waitSemaphores.end(),
		std::make_move_iterator(other.waitSemaphores.begin()),
		std::make_move_iterator(other.waitSemaphores.end()));
	swapchains.insert(
		swapchains.end(),
		std::make_move_iterator(other.swapchains.begin()),
		std::make_move_iterator(other.swapchains.end()));
	imageIndices.insert(
		imageIndices.end(),
		std::make_move_iterator(other.imageIndices.begin()),
		std::make_move_iterator(other.imageIndices.end()));
	results.insert(
		results.end(),
		std::make_move_iterator(other.results.begin()),
		std::make_move_iterator(other.results.end()));
	presentIds.insert(
		presentIds.end(),
		std::make_move_iterator(other.presentIds.begin()),
		std::make_move_iterator(other.presentIds.end()));
	
	return *this;
}

template <GraphicsApi G>
QueuePresentInfo<G> operator|(QueuePresentInfo<G>&& lhs, QueuePresentInfo<G>&& rhs)
{
	return std::forward<QueuePresentInfo<G>>(lhs |= std::forward<QueuePresentInfo<G>>(rhs));
}

template <GraphicsApi G>
void Queue<G>::SwapAndResetPool()
{
	ENSURE(myPendingSubmits.empty());
	ENSURE(myPendingPresent.waitSemaphores.empty());
	ENSURE(myPendingPresent.swapchains.empty());
	ENSURE(myPendingPresent.imageIndices.empty());
	ENSURE(myPendingPresent.results.empty());
	ENSUREF(
		myPools[0].GetOutstandingCount() == 0 && myPools[1].GetOutstandingCount() == 0,
		"command buffers held over a pool switch: submit (or drop) them first");

	myPools[0].Swap(myPools[1]);
	myPools[0].Reset();
}

// the submissions enqueued and the timeline callbacks: the same for every backend (Submit hands them to the backend)

template <GraphicsApi G>
bool Queue<G>::SubmitCallbacks(core::TaskExecutor& executor, uint64_t timelineValue) const
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

template <GraphicsApi G>
void Queue<G>::EnqueueSubmit(QueueDeviceSyncInfo<G>&& syncInfo, std::vector<CommandBuffer<G>>&& commands)
{
	ZoneScopedN("Queue::EnqueueSubmit");

	QueueSubmitInfo<G> submitInfo{std::move(syncInfo), {}, 0};
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

template <GraphicsApi G>
void Queue<G>::EnqueueSubmit(QueueDeviceSyncInfo<G>&& syncInfo, CommandBuffer<G>&& commands)
{
	std::vector<CommandBuffer<G>> list;
	list.push_back(std::move(commands));
	EnqueueSubmit(std::move(syncInfo), std::move(list));
}

template <GraphicsApi G>
void Queue<G>::EnqueueSubmit(QueueDeviceSyncInfo<G>&& syncInfo)
{
	EnqueueSubmit(std::move(syncInfo), std::vector<CommandBuffer<G>>{});
}

} // namespace rhi

#include "vulkan/queue.inl"
