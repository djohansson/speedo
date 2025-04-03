template <GraphicsApi G>
QueueSyncInfo<G>& QueueSyncInfo<G>::operator|=(QueueSyncInfo<G>&& other)
{
	waitFences.insert(
		waitFences.end(),
		std::make_move_iterator(other.waitFences.begin()),
		std::make_move_iterator(other.waitFences.end()));
	waitSemaphores.insert(
		waitSemaphores.end(),
		std::make_move_iterator(other.waitSemaphores.begin()),
		std::make_move_iterator(other.waitSemaphores.end()));
	maxTimelineValue = std::max(maxTimelineValue, other.maxTimelineValue);

	return *this;
}

template <GraphicsApi G>
QueuePresentInfo<G>& QueuePresentInfo<G>::operator|=(QueuePresentInfo<G>&& other)
{
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
	
	QueueSyncInfo<G>::operator|=(std::move(other));
	
	return *this;
}

template <GraphicsApi G>
QueueSyncInfo<G> operator|(QueueSyncInfo<G>&& lhs, QueueSyncInfo<G>&& rhs)
{
	return std::forward<QueueSyncInfo<G>>(lhs |= std::forward<QueueSyncInfo<G>>(rhs));
}

template <GraphicsApi G>
QueuePresentInfo<G> operator|(QueuePresentInfo<G>&& lhs, QueuePresentInfo<G>&& rhs)
{
	return std::forward<QueuePresentInfo<G>>(lhs |= std::forward<QueuePresentInfo<G>>(rhs));
}

#include "vulkan/queue.inl"
