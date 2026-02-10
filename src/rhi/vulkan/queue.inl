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

template <>
template <SourceLocationData Location>
std::shared_ptr<void> Queue<kVk>::CreateGpuScope(CommandBufferHandle<kVk> cmd)
{
	return InternalGpuScope(cmd, Location);
}
