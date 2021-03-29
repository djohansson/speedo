template <>
template <typename... Args>
void QueueContext<Vk>::enqueueSubmit(Args&&... args)
{
    myPendingSubmits.emplace_back(std::move(args)...);
}

template <>
template <typename T, typename... Ts>
void QueueContext<Vk>::enqueuePresent(T&& first, Ts&&... rest)
{
    myPendingPresent |= std::move(first);

    if constexpr (sizeof...(rest) > 0)
        enqueuePresent(std::forward<Ts>(rest)...);
}

template <>
template <typename T, uint32_t Line>
std::shared_ptr<void> QueueContext<Vk>::trace(CommandBufferHandle<Vk> cmd, const char* function, const char* file)
{
    if constexpr (PROFILING_ENABLED)
    {
        if (!myDesc.tracingEnableInitCmd)
            return {};

        static const auto srcLoc = SourceLocationData{T::getTypeName(), function, file, Line};

        return internalTrace(cmd, srcLoc);
    }

    return {};
}
