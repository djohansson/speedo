template <GraphicsApi G>
QueueHostSyncInfo<G>& QueueHostSyncInfo<G>::operator|=(QueueHostSyncInfo<G>&& other)
{
	fences.insert(
		fences.end(),
		std::make_move_iterator(other.fences.begin()),
		std::make_move_iterator(other.fences.end()));
	presentIds.insert(
		presentIds.end(),
		std::make_move_iterator(other.presentIds.begin()),
		std::make_move_iterator(other.presentIds.end()));
	retiredSemaphores.insert(
		retiredSemaphores.end(),
		std::make_move_iterator(other.retiredSemaphores.begin()),
		std::make_move_iterator(other.retiredSemaphores.end()));
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
	
	return *this;
}

template <GraphicsApi G>
QueuePresentInfo<G> operator|(QueuePresentInfo<G>&& lhs, QueuePresentInfo<G>&& rhs)
{
	return std::forward<QueuePresentInfo<G>>(lhs |= std::forward<QueuePresentInfo<G>>(rhs));
}

#include "vulkan/queue.inl"
