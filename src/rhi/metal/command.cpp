// the Metal backend's command pools and command buffers: Metal's are one-shot, so a pool's Begin makes a new one from the
// pool's queue, and its Reset (once the gpu is done with what it held, see Queue::SwapAndResetPool) frees those it gave
#include <rhi/metal/backend.h>

#include <rhi/command.h>
#include <rhi/device.h>
#include <rhi/rhi.h>

#include <core/assert.h>
#include <core/profiling.h>

#include <print>
#include <utility>

namespace rhi
{

namespace metal
{

void CommandBufferObject::EndEncoders()
{
	if (computeEncoder != nullptr)
	{
		computeEncoder->endEncoding();
		computeEncoder->release();
		computeEncoder = nullptr;
	}
	if (blitEncoder != nullptr)
	{
		blitEncoder->endEncoding();
		blitEncoder->release();
		blitEncoder = nullptr;
	}
	if (renderEncoder != nullptr)
	{
		renderEncoder->endEncoding();
		renderEncoder->release();
		renderEncoder = nullptr;
	}
}

MTL::RenderCommandEncoder* CommandBufferObject::BeginRenderPass(MTL::RenderPassDescriptor* descriptor)
{
	EndEncoders();
	NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
	renderEncoder = commandBuffer->renderCommandEncoder(descriptor)->retain();
	pool->release();
	// a new encoder's state: its defaults
	viewport.reset();
	scissor.reset();
	cullMode = MTL::CullModeNone;
	stencilReference = 0;
	return renderEncoder;
}

MTL::RenderCommandEncoder* CommandBufferObject::RenderEncoder() const
{
	auto* encoder = primary != nullptr ? primary->renderEncoder : renderEncoder;
	ENSUREF(encoder != nullptr, "a draw outside a render pass (the render target's Begin)");
	return encoder;
}

MTL::ComputeCommandEncoder* CommandBufferObject::ComputeEncoder()
{
	if (computeEncoder == nullptr)
	{
		EndEncoders();
		NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
		computeEncoder = commandBuffer->computeCommandEncoder()->retain();
		pool->release();
	}
	return computeEncoder;
}

MTL::BlitCommandEncoder* CommandBufferObject::BlitEncoder()
{
	if (blitEncoder == nullptr)
	{
		EndEncoders();
		NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
		blitEncoder = commandBuffer->blitCommandEncoder()->retain();
		pool->release();
	}
	return blitEncoder;
}

} // namespace metal

namespace commandpool
{

// the kinds of command buffer a pool keeps apart (indices into its lists)
constexpr size_t kPrimary = 0;
constexpr size_t kSecondary = 1;

void Free(metal::CommandBufferObject* object)
{
	object->EndEncoders();
	for (auto* transient : object->transients)
		transient->release();
	if (object->commandBuffer != nullptr)
		object->commandBuffer->release();
	delete object; //NOLINT(cppcoreguidelines-owning-memory): the handle is the pool's
}

} // namespace commandpool

IMPLEMENT_OBJECT_GETINSTANCE(CommandPool<kMetal>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(CommandPool<kMetal>);

// defined before CommandBuffer's members, which use it (see CommandBuffer::Release)
template <>
void CommandPool<kMetal>::Retire(CommandBufferHandle<kMetal> handle, bool secondary)
{
	using namespace commandpool;

	ENSURE(myOutstanding > 0);
	myOutstanding--;
	myUsed[secondary ? kSecondary : kPrimary].push_back(handle);
}

template <>
void CommandBuffer<kMetal>::End()
{
	ZoneScopedN("CommandBuffer::End");

	ENSURE(myRecording);
	myRecording = false;
	myHandle->EndEncoders();
}

template <>
CommandBuffer<kMetal>::~CommandBuffer()
{
	if (!IsValid())
		return;

#if (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
	std::println(stderr, "a command buffer was dropped without being submitted: its commands won't run");
#endif
	(void)Release();
}

template <>
CommandPool<kMetal>::CommandPool(CreateDescType&& desc, CommandPoolHandle<kMetal>&& pool)
	: DeviceObject<CommandPool<kMetal>>(std::forward<CreateDescType>(desc))
	, myPool(std::forward<CommandPoolHandle<kMetal>>(pool))
{
	ENSUREF(myPool->queue != nullptr, "a Metal command pool needs its queue (CommandPoolCreateDesc::queue)");
}

template <>
CommandPool<kMetal>::CommandPool(CreateDescType&& desc)
	: CommandPool(
		std::forward<CreateDescType>(desc),
		new metal::CommandPoolObject{.queue = reinterpret_cast<MTL::CommandQueue*>(desc.queue)}) //NOLINT(cppcoreguidelines-owning-memory)
{}

template <>
void CommandPool<kMetal>::Swap(CommandPool& other) noexcept
{
	DeviceObject<CommandPool<kMetal>>::Swap(other);
	std::swap(myPool, other.myPool);
	std::swap(myFree, other.myFree);
	std::swap(myUsed, other.myUsed);
	std::swap(myOutstanding, other.myOutstanding);
}

template <>
CommandPool<kMetal>::CommandPool(CommandPool&& other) noexcept
{
	Swap(other);
}

template <>
CommandPool<kMetal>& CommandPool<kMetal>::operator=(CommandPool&& other) noexcept
{
	Swap(other);
	return *this;
}

template <>
CommandPool<kMetal>::~CommandPool()
{
	if (!IsValid())
		return;

	for (auto& handles : myUsed)
		for (auto* handle : handles)
			commandpool::Free(handle);
	for (auto& handles : myFree)
		for (auto* handle : handles)
			commandpool::Free(handle);
	delete myPool; //NOLINT(cppcoreguidelines-owning-memory)
}

template <>
void CommandPool<kMetal>::Reset()
{
	ZoneScopedN("CommandPool::Reset");

	ENSUREF(myOutstanding == 0, "resetting a command pool while {} of its command buffers are held", myOutstanding);

	for (auto& handles : myUsed)
	{
		for (auto* handle : handles)
			commandpool::Free(handle);
		handles.clear();
	}
}

template <>
CommandBufferHandle<kMetal> CommandPool<kMetal>::InternalTake(bool /*secondary*/)
{
	auto* object = new metal::CommandBufferObject(); //NOLINT(cppcoreguidelines-owning-memory): freed by Reset
	NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
	object->commandBuffer = myPool->queue->commandBuffer()->retain();
	object->commandBuffer->setLabel(NS::String::string(std::string(GetName()).c_str(), NS::UTF8StringEncoding));
	pool->release();
	myOutstanding++;
	return object;
}

template <>
CommandBuffer<kMetal> CommandPool<kMetal>::Begin()
{
	return CommandBuffer<kMetal>(*this, InternalTake(false), false);
}

template <>
CommandBufferHandle<kMetal> CommandPool<kMetal>::Allocate()
{
	return InternalTake(false);
}

template <>
CommandBuffer<kMetal> CommandPool<kMetal>::BeginSecondary(const RenderTargetBeginInfo<kMetal>& renderTarget)
{
	// recorded inline, into the primary's open render encoder (see CommandBufferObject::primary)
	const auto* rendering = std::get_if<DynamicRenderingInfo<kMetal>>(&renderTarget);
	ENSUREF(rendering != nullptr && rendering->renderInfo.primary != nullptr, "a secondary command buffer outside a render target's pass");
	auto* object = new metal::CommandBufferObject(); //NOLINT(cppcoreguidelines-owning-memory): freed by Reset
	object->primary = rendering->renderInfo.primary;
	myOutstanding++;
	return CommandBuffer<kMetal>(*this, object, true);
}

} // namespace rhi
