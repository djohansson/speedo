// the Metal backend's command pools and command buffers: Metal's are one-shot, so a pool's Begin makes a new one from the
// pool's queue, and its Reset (once the gpu is done with what it held, see Queue::SwapAndResetPool) frees those it gave
#include <rhi/metal/backend.h>

#include <rhi/command.h>
#include <rhi/crashreport.h>
#include <rhi/device.h>
#include <rhi/rhi.h>

#include <core/assert.h>
#include <core/profiling.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <format>
#include <print>
#include <ranges>
#include <string>
#include <string_view>
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
	auto* open = OpenEncoder();
	if (open == nullptr)
		return;
	// the passes' debug groups pushed on it popped first, innermost first (they must balance within it)
	for (auto& breadcrumb : std::ranges::reverse_view(breadcrumbs))
		if (breadcrumb.encoder == open)
		{
			open->popDebugGroup();
			breadcrumb.encoder = nullptr;
		}
	open->endEncoding();
	open->release();
	computeEncoder = nullptr;
	blitEncoder = nullptr;
	accelerationStructureEncoder = nullptr;
	renderEncoder = nullptr;
	lastSamples = std::exchange(openSamples, std::nullopt);
}

MTL::CommandEncoder* CommandBufferObject::OpenEncoder() const
{
	// one at most is open
	const std::array<MTL::CommandEncoder*, 4> encoders{computeEncoder, blitEncoder, accelerationStructureEncoder, renderEncoder};
	const auto open = std::ranges::find_if(encoders, [](const MTL::CommandEncoder* encoder) { return encoder != nullptr; });
	return open != encoders.end() ? *open : nullptr;
}

void CommandBufferObject::LabelEncoder(MTL::CommandEncoder* encoder) const
{
	if (breadcrumbs.empty())
		return;
	std::string label;
	for (const auto& breadcrumb : breadcrumbs)
		label += (label.empty() ? "" : " > ") + breadcrumb.name;
	encoder->setLabel(NS::String::string(label.c_str(), NS::UTF8StringEncoding));
}

void CommandBufferObject::BeginBreadcrumb(std::string_view name)
{
	if (primary != nullptr)
	{
		primary->BeginBreadcrumb(name);
		return;
	}
	auto* open = OpenEncoder();
	if (open != nullptr)
		open->pushDebugGroup(NS::String::string(std::string(name).c_str(), NS::UTF8StringEncoding));
	breadcrumbs.push_back({.name = std::string(name), .encoder = open});
}

void CommandBufferObject::EndBreadcrumb()
{
	if (primary != nullptr)
	{
		primary->EndBreadcrumb();
		return;
	}
	ENSURE(!breadcrumbs.empty());
	if (breadcrumbs.back().encoder != nullptr && breadcrumbs.back().encoder == OpenEncoder())
		breadcrumbs.back().encoder->popDebugGroup();
	breadcrumbs.pop_back();
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
		NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
		renderEncoder = commandBuffer->renderCommandEncoder(pendingRenderPass)->retain();
		LabelEncoder(renderEncoder);
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
		LabelEncoder(computeEncoder);
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
		LabelEncoder(blitEncoder);
		descriptor->release();
		pool->release();
	}
	return blitEncoder;
}

MTL::AccelerationStructureCommandEncoder* CommandBufferObject::AccelerationStructureEncoder()
{
	if (accelerationStructureEncoder == nullptr)
	{
		EndEncoders();
		NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
		auto* descriptor = MTL::AccelerationStructurePassDescriptor::alloc()->init();
		if (auto samples = BeginSamples())
		{
			auto* attachment = descriptor->sampleBufferAttachments()->object(0);
			attachment->setSampleBuffer(samples->buffer);
			attachment->setStartOfEncoderSampleIndex(samples->start);
			attachment->setEndOfEncoderSampleIndex(samples->start + 1);
		}
		accelerationStructureEncoder = commandBuffer->accelerationStructureCommandEncoder(descriptor)->retain();
		LabelEncoder(accelerationStructureEncoder);
		descriptor->release();
		pool->release();
	}
	return accelerationStructureEncoder;
}

std::optional<TimestampSample> CommandBufferObject::Timestamp(QueryPoolObject& pool)
{
	if (primary != nullptr)
		return primary->Timestamp(pool);
	if (computeEncoder != nullptr || blitEncoder != nullptr || accelerationStructureEncoder != nullptr || renderEncoder != nullptr)
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
// or was affected (its label, the error state, its breadcrumbs), printed and then the crash report's (rhi::CrashFatal: a
// failed command buffer is Vulkan's lost device, its signals never run), which ends the process
void ReportErrors(MTL::CommandBuffer* commandBuffer)
{
	commandBuffer->addCompletedHandler([](MTL::CommandBuffer* completed)
	{
		if (completed->status() != MTL::CommandBufferStatusError)
			return;
		auto text = [](NS::String* string) { return string != nullptr ? string->utf8String() : "(none)"; };
		NS::Error* error = completed->error();
		const std::string result = error != nullptr ? std::format("{} (code {})", text(error->localizedDescription()), error->code()) : "(no error)";
		std::string what = std::format("command buffer \"{}\" failed", text(completed->label()));
		std::println(stderr, "Metal: {}: {}", what, result);
		auto* infos = error != nullptr && error->userInfo() != nullptr ? error->userInfo()->object<NS::Array>(MTL::CommandBufferEncoderInfoErrorKey) : nullptr;
		for (NS::UInteger infoIt = 0; infos != nullptr && infoIt < infos->count(); infoIt++)
		{
			auto* info = infos->object<MTL::CommandBufferEncoderInfo>(infoIt);
			if (info->errorState() != MTL::CommandEncoderErrorStateFaulted && info->errorState() != MTL::CommandEncoderErrorStateAffected)
				continue;
			// its label (the passes it was begun in) and debug signposts (those begun while it was open): the breadcrumbs
			std::string signposts;
			for (NS::UInteger signpostIt = 0; info->debugSignposts() != nullptr && signpostIt < info->debugSignposts()->count(); signpostIt++)
				signposts += std::format("{}{}", signposts.empty() ? "" : ", ", text(info->debugSignposts()->object<NS::String>(signpostIt)));
			auto encoder = std::format(
				"encoder \"{}\": {}{}",
				text(info->label()),
				info->errorState() == MTL::CommandEncoderErrorStateFaulted ? "faulted" : "affected",
				signposts.empty() ? "" : std::format(" (in {})", signposts));
			std::println(stderr, "Metal:   {}", encoder);
			what += "; " + encoder;
		}
		std::fflush(stderr);
		CrashFatal(result, what);
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
