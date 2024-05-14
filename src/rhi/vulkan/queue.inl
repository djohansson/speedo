template <>
template <typename T, typename... Ts>
void Queue<Vk>::EnqueueSubmit(T&& first, Ts&&... rest)
{
	ZoneScopedN("Queue::EnqueueSubmit");

	myPendingSubmits.emplace_back(InternalPrepareSubmit(std::forward<T>(first)));

	if constexpr (sizeof...(rest) > 0)
		EnqueueSubmit(std::forward<Ts>(rest)...);
}

template <>
template <typename T, typename... Ts>
void Queue<Vk>::EnqueuePresent(T&& first, Ts&&... rest)
{
	ZoneScopedN("Queue::EnqueuePresent");

	myPendingPresent |= std::forward<T>(first);

	if constexpr (sizeof...(rest) > 0)
		EnqueuePresent(std::forward<Ts>(rest)...);
}


#if (PROFILING_LEVEL > 0)
template <>
template <SourceLocationData Location>
std::shared_ptr<void> Queue<Vk>::GpuScope(CommandBufferHandle<Vk> cmd)
{
	return InternalGpuScope(cmd, Location);
}
#endif
