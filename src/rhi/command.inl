template <GraphicsApi G>
CommandBufferAccessScope<G>
CommandPoolContext<G>::commands(const CommandBufferAccessScopeDesc<G>& beginInfo)
{
	if (myRecordingCommands[beginInfo.level] &&
		myRecordingCommands[beginInfo.level].value().getDesc() == beginInfo)
		return internalCommands(beginInfo);
	else
		return internalBeginScope(beginInfo);
}

template <GraphicsApi G>
void CommandPoolContext<G>::internalEndCommands(CommandBufferLevel<G> level)
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
	, myIndex(myDesc.scopedBeginEnd ? myArray->begin(beginInfo) : 0)
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
		myArray->recording(myIndex))
		myArray->end(myIndex);
}

template <GraphicsApi G>
CommandBufferAccessScope<G>& CommandBufferAccessScope<G>::operator=(CommandBufferAccessScope other)
{
	swap(other);
	return *this;
}

template <GraphicsApi G>
void CommandBufferAccessScope<G>::swap(CommandBufferAccessScope& rhs) noexcept
{
	std::swap(myDesc, rhs.myDesc);
	std::swap(myRefCount, rhs.myRefCount);
	std::swap(myArray, rhs.myArray);
	std::swap(myIndex, rhs.myIndex);
}
