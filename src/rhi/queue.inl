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
	callbacks.insert(
		callbacks.end(),
		std::make_move_iterator(other.callbacks.begin()),
		std::make_move_iterator(other.callbacks.end()));
	
	return *this;
}

template <GraphicsApi G>
QueuePresentInfo<G> operator|(QueuePresentInfo<G>&& lhs, QueuePresentInfo<G>&& rhs)
{
	return std::forward<QueuePresentInfo<G>>(lhs |= std::forward<QueuePresentInfo<G>>(rhs));
}

#include "vulkan/queue.inl"
