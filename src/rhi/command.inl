namespace rhi
{

template <GraphicsApi G>
CommandBufferAccessScope<G>
CommandPool<G>::Commands(const CommandBufferAccessScopeDesc<G>& beginInfo)
{
	// a new one if none is recording (or the one there was ended: never hand out an ended buffer)
	//NOLINTBEGIN(bugprone-unchecked-optional-access)
	if (!myRecordingCommands[beginInfo.level].has_value() || myRecordingCommands[beginInfo.level].value().GetDesc() != beginInfo ||
		!myRecordingCommands[beginInfo.level].value().IsRecording())
		return InternalBeginScope(beginInfo);
	//NOLINTEND(bugprone-unchecked-optional-access)
	
	return myRecordingCommands[beginInfo.level].value();
}

template <GraphicsApi G>
void CommandPool<G>::InternalEndCommands(uint8_t level)
{
	if (myRecordingCommands[level])
		myRecordingCommands[level] = std::nullopt;
}

template <GraphicsApi G>
CommandBufferAccessScope<G>::CommandBufferAccessScope(
	const CommandBufferAccessScopeDesc<G>& beginInfo, CommandBufferArray<G>* array)
	: myDesc(beginInfo)
	, myRefCount(std::make_shared<uint32_t>(1))
	, myArray(array)
	, myIndex(myDesc.scopedBeginEnd ? myArray->Begin(beginInfo) : 0)
{}

template <GraphicsApi G>
void CommandBufferAccessScope<G>::End() const
{
	// the scope itself, and the pool's copy if it is the pool's recording one: anyone else holding it would record on
	// into the ended buffer (KosmicKrisp crashes creating its next encoder). record what is submitted right away with
	// CommandPool::OneShotCommands instead
	ENSUREF(
		!myRefCount || *myRefCount <= 2,
		"ending a command buffer others are recording into ({} holders): use CommandPool::OneShotCommands for what is "
		"submitted right away",
		*myRefCount);
	myArray->End(myIndex);
}

template <GraphicsApi G>
CommandBufferAccessScope<G>::CommandBufferAccessScope(const CommandBufferAccessScope& other) noexcept
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
CommandBufferAccessScope<G>& CommandBufferAccessScope<G>::operator=(CommandBufferAccessScope&& other) noexcept
{
	myDesc = std::exchange(other.myDesc, {});
	myRefCount = std::exchange(other.myRefCount, {});
	myArray = std::exchange(other.myArray, {});
	myIndex = std::exchange(other.myIndex, {});
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

} // namespace rhi
