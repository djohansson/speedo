template <>
template <typename... Args>
void Queue<Vk>::enqueueSubmit(Args&&... args)
{
    myPendingSubmits.emplace_back(std::move(args)...);
}

template <>
template <typename T, typename... Ts>
void Queue<Vk>::enqueuePresent(T&& first, Ts&&... rest)
{
    myPendingPresent ^= std::move(first);

    if constexpr (sizeof...(rest) > 0)
        enqueuePresent(std::forward<Ts>(rest)...);
}

template <>
template <typename Function>
std::shared_ptr<void> Queue<Vk>::trace(CommandBufferHandle<Vk> cmd, const char* name, const char* function, const char* file, uint32_t line)
{
    if (!myDesc.tracingEnabled)
        return {};
    
    static constexpr Function* dummy = nullptr;
    (void)dummy;

    static const auto srcLoc = SourceLocationData{name, function, file, line};

    return internalTrace(cmd, srcLoc);
}
