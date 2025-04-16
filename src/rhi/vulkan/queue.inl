template <>
template <typename T, typename... Ts>
void Queue<kVk>::EnqueueSubmit(T&& first, Ts&&... rest)
{
	ZoneScopedN("Queue::EnqueueSubmit");

	myPendingSubmits.emplace_back(InternalPrepareSubmit(std::forward<T>(first)));

	if constexpr (sizeof...(rest) > 0)
		EnqueueSubmit(std::forward<Ts>(rest)...);
}

template <>
template <typename T, typename... Ts>
void Queue<kVk>::EnqueuePresent(T&& first, Ts&&... rest)
{
	ZoneScopedN("Queue::EnqueuePresent");

	myPendingPresent |= std::forward<T>(first);

	if constexpr (sizeof...(rest) > 0)
		EnqueuePresent(std::forward<Ts>(rest)...);
}

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
	maxTimelineValue = std::max(maxTimelineValue, other.maxTimelineValue);
	
	return *this;
}

#if (SPEEDO_PROFILING_LEVEL > 0)
template <>
template <SourceLocationData Location>
std::shared_ptr<void> Queue<kVk>::GpuScope(CommandBufferHandle<kVk> cmd)
{
	return InternalGpuScope(cmd, Location);
}
#endif
