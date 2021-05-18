template <>
template <typename... Args>
void QueueContext<Vk>::enqueueSubmit(Args&&... args)
{
    ZoneScopedN("QueueContext::enqueueSubmit");

    myPendingSubmits.emplace_back(std::forward<Args>(args)...);
}

template <>
template <typename T, typename... Ts>
void QueueContext<Vk>::enqueuePresent(T&& first, Ts&&... rest)
{
    ZoneScopedN("QueueContext::enqueuePresent");

    myPendingPresent |= std::forward<T>(first);

    if constexpr (sizeof...(rest) > 0)
        enqueuePresent(std::forward<Ts>(rest)...);
}

template <>
template <typename T, uint32_t Line>
std::shared_ptr<void> QueueContext<Vk>::trace(CommandBufferHandle<Vk> cmd, const char* function, const char* file)
{
    ZoneScopedN("QueueContext::trace");

    if constexpr (PROFILING_ENABLED)
    {
        if (!myDesc.tracingEnableInitCmd)
            return {};

        static const auto srcLoc = SourceLocationData{T::getTypeName(), function, file, Line};

        return internalTrace(cmd, srcLoc);
    }

    return {};
}
