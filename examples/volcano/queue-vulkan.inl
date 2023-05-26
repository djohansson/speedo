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

template <>
template <typename T, uint32_t Line>
std::shared_ptr<void>
Queue<Vk>::trace(CommandBufferHandle<Vk> cmd, const char* function, const char* file)
{
	ZoneScopedN("Queue::trace");

	if constexpr (PROFILING_ENABLED)
	{
		if (!myDesc.tracingEnableInitCmd)
			return {};

		static const auto srcLoc = SourceLocationData{T::getTypeName(), function, file, Line};

		return internalTrace(cmd, srcLoc);
	}

	return {};
}
