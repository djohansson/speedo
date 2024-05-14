template <GraphicsApi G>
CommandBufferAccessScope<G>
CommandPool<G>::Commands(const CommandBufferAccessScopeDesc<G>& beginInfo)
{
	if (myRecordingCommands[beginInfo.level] &&
		myRecordingCommands[beginInfo.level].value().GetDesc() == beginInfo)
		return InternalCommands(beginInfo);
	else
		return InternalBeginScope(beginInfo);
}

template <GraphicsApi G>
void CommandPool<G>::InternalEndCommands(uint8_t level)
{
	if (myRecordingCommands[level])
		myRecordingCommands[level] = std::nullopt;
}

template <GraphicsApi G>
CommandBufferAccessScope<G>::CommandBufferAccessScope(
	CommandBufferArray<G>* array, const CommandBufferAccessScopeDesc<G>& beginInfo)
	: myDesc(beginInfo)
	, myRefCount(std::make_shared<uint32_t>(1))
	, myArray(array)
	, myIndex(myDesc.scopedBeginEnd ? myArray->Begin(beginInfo) : 0)
{}

template <GraphicsApi G>
CommandBufferAccessScope<G>::CommandBufferAccessScope(const CommandBufferAccessScope& other)
	: myDesc(other.myDesc)
	, myRefCount(other.myRefCount)
	, myArray(other.myArray)
	, myIndex(other.myIndex)
{
	(*myRefCount)++;
}

template <GraphicsApi G>
CommandBufferAccessScope<G>::CommandBufferAccessScope(CommandBufferAccessScope&& other) noexcept
	: myDesc(std::exchange(other.myDesc, {}))
	, myRefCount(std::exchange(other.myRefCount, {}))
	, myArray(std::exchange(other.myArray, {}))
	, myIndex(std::exchange(other.myIndex, {}))
{}

template <GraphicsApi G>
CommandBufferAccessScope<G>::~CommandBufferAccessScope()
{
	if (myDesc.scopedBeginEnd && myRefCount && (--(*myRefCount) == 0) &&
		myArray->Recording(myIndex))
		myArray->End(myIndex);
}

template <GraphicsApi G>
CommandBufferAccessScope<G>& CommandBufferAccessScope<G>::operator=(CommandBufferAccessScope other)
{
	Swap(other);
	return *this;
}

template <GraphicsApi G>
void CommandBufferAccessScope<G>::Swap(CommandBufferAccessScope& rhs) noexcept
{
	std::swap(myDesc, rhs.myDesc);
	std::swap(myRefCount, rhs.myRefCount);
	std::swap(myArray, rhs.myArray);
	std::swap(myIndex, rhs.myIndex);
}
