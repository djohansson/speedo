template <>
template <typename T, typename... Ts>
void Queue<Vk>::enqueueSubmit(T&& first, Ts&&... rest)
{
	ZoneScopedN("Queue::enqueueSubmit");

	myPendingSubmits.emplace_back(internalPrepareSubmit(std::forward<T>(first)));

	if constexpr (sizeof...(rest) > 0)
		enqueueSubmit(std::forward<Ts>(rest)...);
}

template <>
template <typename T, typename... Ts>
void Queue<Vk>::enqueuePresent(T&& first, Ts&&... rest)
{
	ZoneScopedN("Queue::enqueuePresent");

	myPendingPresent |= std::forward<T>(first);

	if constexpr (sizeof...(rest) > 0)
		enqueuePresent(std::forward<Ts>(rest)...);
}


#if (PROFILING_LEVEL > 0)
template <>
template <SourceLocationData Location>
std::shared_ptr<void> Queue<Vk>::gpuScope(CommandBufferHandle<Vk> cmd)
{
	return internalGpuScope(cmd, Location);
}
#endif
