template <GraphicsBackend B>
CommandBufferAccessScope<B> CommandPoolContext<B>::commands(const CommandBufferAccessScopeDesc<B>& beginInfo)
{
    if (myRecordingCommands[beginInfo.level] && myRecordingCommands[beginInfo.level].value().getDesc() == beginInfo)
        return internalCommands(beginInfo);
    else
        return internalBeginScope(beginInfo);
}

template <GraphicsBackend B>
void CommandPoolContext<B>::internalEndCommands(CommandBufferLevel<B> level)
{
    if (myRecordingCommands[level])
        myRecordingCommands[level] = std::nullopt;
}

template <GraphicsBackend B>
CommandBufferAccessScope<B>::CommandBufferAccessScope(
    CommandBufferArray<B>* array,
    const CommandBufferAccessScopeDesc<B>& beginInfo)
: myDesc(beginInfo)
, myRefCount(std::make_shared<uint32_t>(1))
, myArray(array)
, myIndex(myDesc.scopedBeginEnd ? myArray->begin(beginInfo) : 0)
{
}

template <GraphicsBackend B>
CommandBufferAccessScope<B>::CommandBufferAccessScope(const CommandBufferAccessScope& other)
: myDesc(other.myDesc)
, myRefCount(other.myRefCount)
, myArray(other.myArray)
, myIndex(other.myIndex)
{
    (*myRefCount)++;
}

template <GraphicsBackend B>
CommandBufferAccessScope<B>::CommandBufferAccessScope(CommandBufferAccessScope&& other) noexcept
: myDesc(std::exchange(other.myDesc, {}))
, myRefCount(std::exchange(other.myRefCount, {}))
, myArray(std::exchange(other.myArray, {}))
, myIndex(std::exchange(other.myIndex, {}))
{
}

template <GraphicsBackend B>
CommandBufferAccessScope<B>::~CommandBufferAccessScope()
{
    if (myDesc.scopedBeginEnd && myRefCount && (--(*myRefCount) == 0) && myArray->recording(myIndex))
        myArray->end(myIndex);
}

template <GraphicsBackend B>
CommandBufferAccessScope<B>& CommandBufferAccessScope<B>::operator=(CommandBufferAccessScope other)
{
    swap(other);
    return *this;
}

template <GraphicsBackend B>
void CommandBufferAccessScope<B>::swap(CommandBufferAccessScope& rhs) noexcept
{
    std::swap(myDesc, rhs.myDesc);
    std::swap(myRefCount, rhs.myRefCount);
    std::swap(myArray, rhs.myArray);
    std::swap(myIndex, rhs.myIndex);
}
