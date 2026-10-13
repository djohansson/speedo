#include <core/assert.h>

#include <utility>

namespace rhi
{

template <GraphicsApi G>
CommandBuffer<G>::CommandBuffer(CommandPool<G>& pool, CommandBufferHandle<G> handle, bool secondary) noexcept
	: myPool(&pool)
	, myHandle(handle)
	, mySecondary(secondary)
	, myRecording(true)
{}

template <GraphicsApi G>
void CommandBuffer<G>::InternalSwap(CommandBuffer& other) noexcept
{
	std::swap(myPool, other.myPool);
	std::swap(myHandle, other.myHandle);
	std::swap(mySecondary, other.mySecondary);
	std::swap(myRecording, other.myRecording);
}

template <GraphicsApi G>
CommandBuffer<G>::CommandBuffer(CommandBuffer&& other) noexcept
{
	InternalSwap(other);
}

template <GraphicsApi G>
CommandBuffer<G>& CommandBuffer<G>::operator=(CommandBuffer&& other) noexcept
{
	CommandBuffer(std::move(other)).InternalSwap(*this);
	return *this;
}

template <GraphicsApi G>
CommandBufferHandle<G> CommandBuffer<G>::Release()
{
	ENSURE(IsValid());

	if (myRecording)
		End();

	auto handle = std::exchange(myHandle, CommandBufferHandle<G>{});
	std::exchange(myPool, nullptr)->Retire(handle, mySecondary);
	return handle;
}

} // namespace rhi
