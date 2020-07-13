template <GraphicsBackend B>
auto CommandContext<B>::commands(CommandContextBeginInfo<B>&& beginInfo)
{
    if (myRecordingCommands && (*myRecordingCommands).getBeginInfo() == beginInfo)
    {
        std::shared_lock readLock(myCommandsMutex);
        return internalCommands();
    }
    else
    {
        std::unique_lock writeLock(myCommandsMutex);
        return internalBeginScope(std::move(beginInfo));
    }
}
template <GraphicsBackend B>
void CommandContext<B>::endCommands()
{
    if (myRecordingCommands)
        myRecordingCommands = std::nullopt;
}
