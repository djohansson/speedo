template <>
template <typename... Args>
void Queue<Vk>::enqueueSubmit(Args&&... args)
{
	ZoneScopedN("Queue::enqueueSubmit");

	myPendingSubmits.emplace_back(std::forward<Args>(args)...);
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


#if PROFILING_ENABLED
template <>
template <SourceLocationData Location>
std::shared_ptr<void> Queue<Vk>::gpuScope(CommandBufferHandle<Vk> cmd)
{
	static const auto location = Location;
	return internalGpuScope(cmd, location);
}
#endif
