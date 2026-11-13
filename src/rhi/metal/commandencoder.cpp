// the Metal backend's commands: a dispatch on the command buffer's compute encoder, with what the pipeline bound for it
// (the pipeline state, the push constants and each set's argument buffer at the slots its layout gives them, a pushed
// set's by setBytes, and what the sets reference made resident), barriers between dispatches (the resources are hazard tracked: see
// docs/backends-plan.md), and copies between buffers and images on its blit encoder
#include <rhi/metal/backend.h>

#include <rhi/buffer.h>
#include <rhi/command.h>
#include <rhi/commandencoder.h>
#include <rhi/image.h>

#include <core/assert.h>
#include <core/profiling.h>

#include <algorithm>
#include <array>
#include <utility>

namespace rhi
{

namespace commandencoder
{

// what the pipeline bound, set on the compute encoder before a dispatch
static MTL::ComputeCommandEncoder* PrepareDispatch(metal::CommandBufferObject& cmd)
{
	ENSUREF(cmd.computePipeline != nullptr, "a dispatch without a compute pipeline bound (Pipeline::BindPipelineAuto)");
	auto* encoder = cmd.ComputeEncoder();
	encoder->setComputePipelineState(cmd.computePipeline);
	if (!cmd.pushConstants.empty())
		encoder->setBytes(cmd.pushConstants.data(), cmd.pushConstants.size(), cmd.pushConstantSlot);
	for (NS::UInteger slot = 0; slot < cmd.sets.size(); slot++)
	{
		const auto& binding = cmd.sets[slot];
		if (binding.argumentBuffer != nullptr)
			encoder->setBuffer(binding.argumentBuffer, binding.offset, slot);
		else if (!binding.pushed.empty())
			encoder->setBytes(binding.pushed.data(), binding.pushed.size(), slot);
		else
			continue;
		for (auto* resource : binding.resources)
			encoder->useResource(resource, MTL::ResourceUsageRead | MTL::ResourceUsageWrite);
	}
	return encoder;
}

// what the pipeline bound, set on the render encoder (the pass's, open) before a draw: the render pipeline and its depth
// stencil state, and for both stages the push constants and each set's argument buffer at their slots, what the sets
// reference made resident for both
static MTL::RenderCommandEncoder* PrepareDraw(metal::CommandBufferObject& cmd)
{
	ENSUREF(cmd.renderPipeline != nullptr, "a draw without a render pipeline bound (Pipeline::BindPipelineAuto)");
	auto* encoder = cmd.RenderEncoder();
	encoder->setRenderPipelineState(cmd.renderPipeline);
	if (cmd.depthStencil != nullptr)
		encoder->setDepthStencilState(cmd.depthStencil);
	if (!cmd.pushConstants.empty())
	{
		encoder->setVertexBytes(cmd.pushConstants.data(), cmd.pushConstants.size(), cmd.pushConstantSlot);
		encoder->setFragmentBytes(cmd.pushConstants.data(), cmd.pushConstants.size(), cmd.pushConstantSlot);
	}
	for (NS::UInteger slot = 0; slot < cmd.sets.size(); slot++)
	{
		const auto& binding = cmd.sets[slot];
		if (binding.argumentBuffer != nullptr)
		{
			encoder->setVertexBuffer(binding.argumentBuffer, binding.offset, slot);
			encoder->setFragmentBuffer(binding.argumentBuffer, binding.offset, slot);
		}
		else if (!binding.pushed.empty())
		{
			encoder->setVertexBytes(binding.pushed.data(), binding.pushed.size(), slot);
			encoder->setFragmentBytes(binding.pushed.data(), binding.pushed.size(), slot);
		}
		else
		{
			continue;
		}
		for (auto* resource : binding.resources)
			encoder->useResource(resource, MTL::ResourceUsageRead | MTL::ResourceUsageWrite, MTL::RenderStageVertex | MTL::RenderStageFragment);
	}
	return encoder;
}

// a texture's format's texel block (for a copy's rows), from its Metal pixel format: rhi's formats that have one
static FormatBlock BlockOf(MTL::Texture* texture)
{
	constexpr std::array kFormats{
		Format::kR8G8B8A8Unorm, Format::kR8G8B8A8Srgb, Format::kB8G8R8A8Unorm, Format::kB8G8R8A8Srgb,
		Format::kA2R10G10B10UnormPack32, Format::kA2B10G10R10UnormPack32, Format::kR16G16B16A16Sfloat, Format::kR16Sfloat,
		Format::kD32Sfloat, Format::kBC1RgbUnorm, Format::kBC1RgbSrgb, Format::kBC3Unorm, Format::kBC3Srgb, Format::kBC4Unorm,
		Format::kBC5Unorm, Format::kBC7Unorm, Format::kBC7Srgb, Format::kR32Sfloat, Format::kR32G32B32A32Sfloat,
		Format::kR16G16Sfloat, Format::kR32G32Sfloat, Format::kR32G32Uint,
	};
	for (auto format : kFormats)
		if (metal::ToMetal(format) == texture->pixelFormat())
			return rhi::BlockOf(format);
	ENSUREF(false, "a copy of a texture of pixel format {}, whose texels aren't one run of bytes", std::to_underlying(texture->pixelFormat()));
	return {};
}

// a copy's rows of blocks, and for a volume its slices (Metal takes 0 for a 2D texture's)
static std::pair<uint64_t, uint64_t> CopyStrides(const FormatBlock& block, MTL::Size size)
{
	const uint64_t bytesPerRow = uint64_t{(size.width + block.width - 1) / block.width} * block.byteCount;
	const uint64_t bytesPerImage = size.depth > 1 ? bytesPerRow * ((size.height + block.height - 1) / block.height) : 0;
	return {bytesPerRow, bytesPerImage};
}

} // namespace commandencoder

template <>
void CommandEncoder<kMetal>::SetViewport(const Viewport& viewport) const
{
	// flipped: rhi's clip space is Vulkan's, y down, Metal's y up (the shaders are the same for both), so the viewport
	// starts at its bottom and goes up (a negative height), as MoltenVK flips Vulkan's
	myCmd->viewport = MTL::Viewport{
		.originX = viewport.x,
		.originY = viewport.y + viewport.height,
		.width = viewport.width,
		.height = -viewport.height,
		.znear = viewport.minDepth,
		.zfar = viewport.maxDepth};
	myCmd->RenderEncoder()->setViewport(*myCmd->viewport);
}

template <>
void CommandEncoder<kMetal>::SetScissor(const Rect& rect) const
{
	myCmd->scissor = MTL::ScissorRect{
		.x = static_cast<NS::UInteger>(std::max(rect.x, 0)),
		.y = static_cast<NS::UInteger>(std::max(rect.y, 0)),
		.width = rect.width,
		.height = rect.height};
	myCmd->RenderEncoder()->setScissorRect(*myCmd->scissor);
}

template <>
void CommandEncoder<kMetal>::SetCullMode(CullMode mode) const
{
	myCmd->cullMode =
		mode == CullMode::kNone ? MTL::CullModeNone : (mode == CullMode::kFront ? MTL::CullModeFront : MTL::CullModeBack);
	myCmd->RenderEncoder()->setCullMode(myCmd->cullMode);
}

template <>
void CommandEncoder<kMetal>::SetFrontFace(FrontFace face) const
{
	myCmd->RenderEncoder()->setFrontFacingWinding(
		face == FrontFace::kCounterClockwise ? MTL::WindingCounterClockwise : MTL::WindingClockwise);
}

template <>
void CommandEncoder<kMetal>::SetStencilReference(uint32_t reference) const
{
	myCmd->stencilReference = reference;
	myCmd->RenderEncoder()->setStencilReferenceValue(reference);
}

template <>
void CommandEncoder<kMetal>::BindIndexBuffer(const Buffer<kMetal>& buffer, uint64_t offset, IndexType type) const
{
	myCmd->indices = {.buffer = buffer, .offset = offset, .wide = type == IndexType::kUint32};
}

template <>
void CommandEncoder<kMetal>::Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) const
{
	ZoneScopedN("CommandEncoder::Draw");

	commandencoder::PrepareDraw(*myCmd)->drawPrimitives(myCmd->primitiveType, firstVertex, vertexCount, instanceCount, firstInstance);
}

template <>
void CommandEncoder<kMetal>::DrawIndexed(
	uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) const
{
	ZoneScopedN("CommandEncoder::DrawIndexed");

	const auto& indices = myCmd->indices;
	ENSUREF(indices.buffer != nullptr, "an indexed draw without an index buffer bound (CommandEncoder::BindIndexBuffer)");
	commandencoder::PrepareDraw(*myCmd)->drawIndexedPrimitives(
		myCmd->primitiveType,
		indexCount,
		indices.wide ? MTL::IndexTypeUInt32 : MTL::IndexTypeUInt16,
		indices.buffer,
		indices.offset + (uint64_t{firstIndex} * (indices.wide ? 4 : 2)),
		instanceCount,
		vertexOffset,
		firstInstance);
}

template <>
void CommandEncoder<kMetal>::DrawIndexedIndirect(const Buffer<kMetal>& buffer, uint64_t offset, uint32_t drawCount, uint32_t stride) const
{
	ZoneScopedN("CommandEncoder::DrawIndexedIndirect");

	// a Metal indirect draw per command (its indexed arguments are laid out as rhi's commands: index count, instance count,
	// first index, vertex offset, first instance, which the vertex stage takes as its base instance), issued from the
	// host: an indirect command buffer would issue them in one call (later)
	const auto& indices = myCmd->indices;
	ENSUREF(indices.buffer != nullptr, "an indexed draw without an index buffer bound (CommandEncoder::BindIndexBuffer)");
	auto* encoder = commandencoder::PrepareDraw(*myCmd);
	for (uint32_t drawIt = 0; drawIt < drawCount; drawIt++)
		encoder->drawIndexedPrimitives(
			myCmd->primitiveType,
			indices.wide ? MTL::IndexTypeUInt32 : MTL::IndexTypeUInt16,
			indices.buffer,
			indices.offset,
			buffer,
			offset + (uint64_t{drawIt} * stride));
}

template <>
void CommandEncoder<kMetal>::DrawIndexedIndirectCount(
	const Buffer<kMetal>& /*buffer*/,
	uint64_t /*offset*/,
	const Buffer<kMetal>& /*countBuffer*/,
	uint64_t /*countOffset*/,
	uint32_t /*maxDrawCount*/,
	uint32_t /*stride*/) const
{
	ENSUREF(false, "Metal has no draw count indirect (DeviceLimits::drawIndirectCount is false: draw the commands' slots instead)");
}

template <>
void CommandEncoder<kMetal>::CopyImageToBuffer(ImageHandle<kMetal> image, Extent2d extent, const Buffer<kMetal>& buffer) const
{
	ZoneScopedN("CommandEncoder::CopyImageToBuffer");

	const MTL::Size size(extent.width, extent.height, 1);
	const auto [bytesPerRow, bytesPerImage] = commandencoder::CopyStrides(commandencoder::BlockOf(image), size);
	myCmd->BlitEncoder()->copyFromTexture(image, 0, 0, MTL::Origin(0, 0, 0), size, buffer, 0, bytesPerRow, bytesPerImage);
}

template <>
void CommandEncoder<kMetal>::CopyImageToBuffer(const Image<kMetal>& image, const Buffer<kMetal>& buffer) const
{
	ZoneScopedN("CommandEncoder::CopyImageToBuffer");

	const auto& extent = image.GetDesc().extent;
	const MTL::Size size(extent.width, extent.height, std::max(extent.depth, 1U));
	const auto [bytesPerRow, bytesPerImage] = commandencoder::CopyStrides(BlockOf(image.GetDesc().format), size);
	myCmd->BlitEncoder()->copyFromTexture(image, 0, 0, MTL::Origin(0, 0, 0), size, buffer, 0, bytesPerRow, bytesPerImage);
}

template <>
void CommandEncoder<kMetal>::CopyBufferToImage(const Buffer<kMetal>& buffer, const Image<kMetal>& image, const Rect& region) const
{
	ZoneScopedN("CommandEncoder::CopyBufferToImage");

	const MTL::Size size(region.width, region.height, 1);
	const auto [bytesPerRow, bytesPerImage] = commandencoder::CopyStrides(BlockOf(image.GetDesc().format), size);
	myCmd->BlitEncoder()->copyFromBuffer(
		buffer, 0, bytesPerRow, bytesPerImage, size, image, 0, 0, MTL::Origin(region.x, region.y, 0));
}

template <>
void CommandEncoder<kMetal>::UpdateBuffer(const Buffer<kMetal>& buffer, uint64_t offset, std::span<const std::byte> data) const
{
	ZoneScopedN("CommandEncoder::UpdateBuffer");

	// the data copied into a buffer of its own (the command buffer's to release, once its commands are done), then on the
	// blit encoder, in order with the other commands
	constexpr size_t kMaxUpdateByteCount = 65536;
	ENSUREF(data.size() <= kMaxUpdateByteCount && data.size() % 4 == 0, "UpdateBuffer takes at most 64 kB, a multiple of 4 bytes ({})", data.size());
	MTL::Buffer* source = myCmd->commandBuffer->device()->newBuffer(data.data(), data.size(), MTL::ResourceStorageModeShared);
	ENSUREF(source != nullptr, "Failed to create a Metal buffer of {} bytes", data.size());
	myCmd->transients.push_back(source);
	myCmd->BlitEncoder()->copyFromBuffer(source, 0, buffer, offset, data.size());
}

template <>
void CommandEncoder<kMetal>::ExecuteCommands(std::vector<CommandBuffer<kMetal>>&& secondaries) const
{
	// their commands were recorded inline, into this command buffer's render encoder (see CommandPool::BeginSecondary):
	// each is ended and handed back to its pool
	for (auto& secondary : secondaries)
	{
		ENSURE(secondary.IsSecondary());
		if (secondary.IsRecording())
			secondary.End();
		(void)secondary.Release();
	}
}

template <>
void CommandEncoder<kMetal>::FillBuffer(const Buffer<kMetal>& buffer, uint64_t offset, uint64_t size, uint32_t value) const
{
	ZoneScopedN("CommandEncoder::FillBuffer");

	MTL::Buffer* target = buffer;
	const uint64_t byteCount = size != 0 ? size : target->length() - offset;
	const auto byte = static_cast<uint8_t>(value & 0xFFU);
	if (value == byte * 0x01010101U)
	{
		// a value of four equal bytes (0, or SHADER_TYPES_OIT_NONE's ~0): the blit encoder's fill
		myCmd->BlitEncoder()->fillBuffer(target, NS::Range(offset, byteCount), byte);
		return;
	}
	// another value: a buffer of it copied in (the command buffer's to release, see UpdateBuffer)
	ENSUREF(byteCount % 4 == 0, "FillBuffer fills a multiple of 4 bytes ({})", byteCount);
	MTL::Buffer* source = myCmd->commandBuffer->device()->newBuffer(byteCount, MTL::ResourceStorageModeShared);
	ENSUREF(source != nullptr, "Failed to create a Metal buffer of {} bytes", byteCount);
	std::fill_n(static_cast<uint32_t*>(source->contents()), byteCount / 4, value);
	myCmd->transients.push_back(source);
	myCmd->BlitEncoder()->copyFromBuffer(source, 0, target, offset, byteCount);
}

template <>
void CommandEncoder<kMetal>::DispatchIndirect(const Buffer<kMetal>& buffer, uint64_t offset) const
{
	ZoneScopedN("CommandEncoder::DispatchIndirect");

	// the thread group counts from the buffer; the group's size the bound pipeline's entry point's (its numthreads)
	const auto& size = myCmd->computeThreadGroupSize;
	commandencoder::PrepareDispatch(*myCmd)->dispatchThreadgroups(buffer, offset, MTL::Size(size[0], size[1], size[2]));
}

// queue family ownership: the Metal backend has one queue (compute and transfer alias it), so nothing changes hands
template <>
void CommandEncoder<kMetal>::ReleaseOwnership(const Buffer<kMetal>&, uint32_t, uint32_t, PipelineStage, Access) const
{}

template <>
void CommandEncoder<kMetal>::AcquireOwnership(const Buffer<kMetal>&, uint32_t, uint32_t, PipelineStage, Access) const
{}

template <>
void CommandEncoder<kMetal>::ReleaseOwnership(const Image<kMetal>&, uint32_t, uint32_t, PipelineStage, Access) const
{}

template <>
void CommandEncoder<kMetal>::AcquireOwnership(const Image<kMetal>&, uint32_t, uint32_t, PipelineStage, Access) const
{}

template <>
void CommandEncoder<kMetal>::WriteAccelerationStructureCompactedSize(
	const AccelerationStructure<kMetal>& /*structure*/, const QueryPool<kMetal>& /*pool*/, uint32_t /*query*/) const
{
	ENSUREF(false, "the Metal backend's acceleration structures come with ray queries (docs/backends-plan.md, step 7)");
}

template <>
void CommandEncoder<kMetal>::CopyBuffer(const Buffer<kMetal>& source, const Buffer<kMetal>& destination, uint64_t size, uint64_t offset) const
{
	ZoneScopedN("CommandEncoder::CopyBuffer");

	myCmd->BlitEncoder()->copyFromBuffer(source, offset, destination, offset, size);
}

template <>
void CommandEncoder<kMetal>::DispatchThreads(const ComputeLaunchParameters& launch, uint32_t threadsX, uint32_t threadsY, uint32_t threadsZ) const
{
	ZoneScopedN("CommandEncoder::DispatchThreads");

	auto* encoder = commandencoder::PrepareDispatch(*myCmd);
	encoder->dispatchThreads(
		MTL::Size(threadsX, threadsY, threadsZ),
		MTL::Size(launch.threadGroupSize[0], launch.threadGroupSize[1], launch.threadGroupSize[2]));
}

template <>
void CommandEncoder<kMetal>::Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) const
{
	ZoneScopedN("CommandEncoder::Dispatch");

	// the group's size the bound pipeline's entry point's (its numthreads: Metal takes it at the dispatch)
	const auto& size = myCmd->computeThreadGroupSize;
	commandencoder::PrepareDispatch(*myCmd)->dispatchThreadgroups(
		MTL::Size(groupCountX, groupCountY, groupCountZ), MTL::Size(size[0], size[1], size[2]));
}

template <>
void CommandEncoder<kMetal>::Barrier(PipelineStage srcStages, Access /*srcAccess*/, PipelineStage dstStages, Access /*dstAccess*/) const
{
	// between dispatches on the compute encoder: its memory barrier. the host's reads after a wait, and what other
	// encoders' work needs (hazard tracked resources), need nothing
	const bool computeToCompute = (srcStages & PipelineStage::kComputeShader) == PipelineStage::kComputeShader &&
								  (dstStages & PipelineStage::kComputeShader) == PipelineStage::kComputeShader;
	if (computeToCompute && myCmd->computeEncoder != nullptr)
		myCmd->computeEncoder->memoryBarrier(MTL::BarrierScopeBuffers | MTL::BarrierScopeTextures);
}

} // namespace rhi
