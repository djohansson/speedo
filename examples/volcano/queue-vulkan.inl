
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
