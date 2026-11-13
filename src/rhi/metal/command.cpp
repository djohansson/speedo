// the Metal backend's command pools and command buffers: Metal's are one-shot, so a pool's Begin makes a new one from the
// pool's queue, and its Reset (once the gpu is done with what it held, see Queue::SwapAndResetPool) frees those it gave
#include <rhi/metal/backend.h>

#include <rhi/command.h>
#include <rhi/device.h>
#include <rhi/rhi.h>

#include <core/assert.h>
#include <core/profiling.h>

#include <cstdio>
#include <print>
#include <utility>

namespace rhi
{

namespace metal
{

void CommandBufferObject::EndEncoders()
{
	// a pass begun but not drawn in: its encoder made now, for what its attachments load and store (a clear)
	if (pendingRenderPass != nullptr)
		(void)RenderEncoder();
	MTL::CommandEncoder* open = computeEncoder != nullptr ? static_cast<MTL::CommandEncoder*>(computeEncoder)
							  : blitEncoder != nullptr	  ? static_cast<MTL::CommandEncoder*>(blitEncoder)
														  : static_cast<MTL::CommandEncoder*>(renderEncoder);
	if (open == nullptr)
		return;
	open->endEncoding();
	open->release();
	computeEncoder = nullptr;
	blitEncoder = nullptr;
	renderEncoder = nullptr;
	lastSamples = std::exchange(openSamples, std::nullopt);
}

std::optional<TimestampSamples> CommandBufferObject::BeginSamples()
{
	lastSamples.reset();
	openSamples = std::exchange(pendingSamples, std::nullopt);
	return openSamples;
}

void CommandBufferObject::BeginRenderPass(MTL::RenderPassDescriptor* descriptor)
{
	EndEncoders();
	pendingRenderPass = descriptor->retain();
	// a new encoder's state: its defaults
	viewport.reset();
	scissor.reset();
	cullMode = MTL::CullModeNone;
	stencilReference = 0;
}

MTL::RenderCommandEncoder* CommandBufferObject::RenderEncoder()
{
	if (primary != nullptr)
		return primary->RenderEncoder();
	if (renderEncoder == nullptr && pendingRenderPass != nullptr)
	{
		// sampled where its vertex stage starts and its fragment stage ends: the pass's work (Apple's gpus sample its
		// stages' boundaries)
		if (auto samples = BeginSamples())
		{
			auto* attachment = pendingRenderPass->sampleBufferAttachments()->object(0);
			attachment->setSampleBuffer(samples->buffer);
			attachment->setStartOfVertexSampleIndex(samples->start);
			attachment->setEndOfVertexSampleIndex(MTL::CounterDontSample);
			attachment->setStartOfFragmentSampleIndex(MTL::CounterDontSample);
			attachment->setEndOfFragmentSampleIndex(samples->start + 1);
		}
		else
			BeginSamples();
		NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
		renderEncoder = commandBuffer->renderCommandEncoder(pendingRenderPass)->retain();
		pool->release();
		pendingRenderPass->release();
		pendingRenderPass = nullptr;
	}
	ENSUREF(renderEncoder != nullptr, "a draw outside a render pass (the render target's Begin)");
	return renderEncoder;
}

MTL::ComputeCommandEncoder* CommandBufferObject::ComputeEncoder()
{
	if (computeEncoder == nullptr)
	{
		EndEncoders();
		NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
		auto* descriptor = MTL::ComputePassDescriptor::alloc()->init();
		if (auto samples = BeginSamples())
		{
			auto* attachment = descriptor->sampleBufferAttachments()->object(0);
			attachment->setSampleBuffer(samples->buffer);
			attachment->setStartOfEncoderSampleIndex(samples->start);
			attachment->setEndOfEncoderSampleIndex(samples->start + 1);
		}
		computeEncoder = commandBuffer->computeCommandEncoder(descriptor)->retain();
		descriptor->release();
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
		auto* descriptor = MTL::BlitPassDescriptor::alloc()->init();
		if (auto samples = BeginSamples())
		{
			auto* attachment = descriptor->sampleBufferAttachments()->object(0);
			attachment->setSampleBuffer(samples->buffer);
			attachment->setStartOfEncoderSampleIndex(samples->start);
			attachment->setEndOfEncoderSampleIndex(samples->start + 1);
		}
		blitEncoder = commandBuffer->blitCommandEncoder(descriptor)->retain();
		descriptor->release();
		pool->release();
	}
	return blitEncoder;
}

std::optional<TimestampSample> CommandBufferObject::Timestamp(QueryPoolObject& pool)
{
	if (primary != nullptr)
		return primary->Timestamp(pool);
	if (computeEncoder != nullptr || blitEncoder != nullptr || renderEncoder != nullptr)
	{
		const bool render = renderEncoder != nullptr;
		if (openSamples)
		{
			const TimestampSample end{.buffer = openSamples->buffer, .index = openSamples->start + 1};
			if (!render)
				EndEncoders();
			return end;
		}
		if (render)
			return std::nullopt;
		EndEncoders();
	}
	if (!pendingSamples)
		pendingSamples = pool.NewPair();
	// the end of the encoder just ended (the next one sampled too, for the work after it)
	if (pendingRenderPass == nullptr && lastSamples)
		return TimestampSample{.buffer = lastSamples->buffer, .index = lastSamples->start + 1};
	return TimestampSample{.buffer = pendingSamples->buffer, .index = pendingSamples->start};
}

} // namespace metal

namespace commandpool
{

// the kinds of command buffer a pool keeps apart (indices into its lists)
constexpr size_t kPrimary = 0;
constexpr size_t kSecondary = 1;

void Free(metal::CommandBufferObject* object)
{
	// a pass never ended (its command buffer dropped): no encoder made for it now
	if (object->pendingRenderPass != nullptr)
	{
		object->pendingRenderPass->release();
		object->pendingRenderPass = nullptr;
	}
	object->EndEncoders();
	for (auto* transient : object->transients)
		transient->release();
	if (object->commandBuffer != nullptr)
		object->commandBuffer->release();
	delete object; //NOLINT(cppcoreguidelines-owning-memory): the handle is the pool's
}

// a command buffer that fails says so once it completes (on Metal's thread): its error, and each encoder that faulted
// or was affected (its label, the error state). its signals don't run, so what waits for them waits forever: the
// client's hang check reports it then (see docs/backends-plan.md, step 7's crash reports)
void ReportErrors(MTL::CommandBuffer* commandBuffer)
{
	commandBuffer->addCompletedHandler([](MTL::CommandBuffer* completed)
	{
		if (completed->status() != MTL::CommandBufferStatusError)
			return;
		auto text = [](NS::String* string) { return string != nullptr ? string->utf8String() : "(none)"; };
		NS::Error* error = completed->error();
		std::println(
			stderr,
			"Metal: command buffer \"{}\" failed: {} (code {})",
			text(completed->label()),
			error != nullptr ? text(error->localizedDescription()) : "(no error)",
			error != nullptr ? error->code() : 0);
		if (error == nullptr || error->userInfo() == nullptr)
			return;
		auto* infos = error->userInfo()->object<NS::Array>(MTL::CommandBufferEncoderInfoErrorKey);
		for (NS::UInteger infoIt = 0; infos != nullptr && infoIt < infos->count(); infoIt++)
		{
			auto* info = infos->object<MTL::CommandBufferEncoderInfo>(infoIt);
			if (info->errorState() == MTL::CommandEncoderErrorStateFaulted || info->errorState() == MTL::CommandEncoderErrorStateAffected)
				std::println(
					stderr,
					"Metal:   encoder \"{}\": {}",
					text(info->label()),
					info->errorState() == MTL::CommandEncoderErrorStateFaulted ? "faulted" : "affected");
		}
		std::fflush(stderr);
	});
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
	// with each encoder's execution status, for what a failed one says (commandpool::ReportErrors)
	auto* descriptor = MTL::CommandBufferDescriptor::alloc()->init();
	descriptor->setErrorOptions(MTL::CommandBufferErrorOptionEncoderExecutionStatus);
	object->commandBuffer = myPool->queue->commandBuffer(descriptor)->retain();
	descriptor->release();
	object->commandBuffer->setLabel(NS::String::string(std::string(GetName()).c_str(), NS::UTF8StringEncoding));
	commandpool::ReportErrors(object->commandBuffer);
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
