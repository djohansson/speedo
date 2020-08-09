template <GraphicsBackend B>
CommandBufferAccessScope<B> CommandContext<B>::commands(const CommandContextBeginInfo<B>& beginInfo)
{
    if (myRecordingCommands[beginInfo.level] && myRecordingCommands[beginInfo.level].value().getBeginInfo() == beginInfo)
        return internalCommands(beginInfo);
    else
        return internalBeginScope(beginInfo);
}

template <GraphicsBackend B>
void CommandContext<B>::internalEndCommands(CommandBufferLevel<B> level)
{
    if (myRecordingCommands[level])
        myRecordingCommands[level] = std::nullopt;
}
