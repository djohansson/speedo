// the Metal backend's commands: a dispatch on the command buffer's compute encoder, with what the pipeline bound for it
// (the pipeline state, the push constants and each set's argument buffer at the slots its layout gives them, a pushed
// set's by setBytes, and what the sets reference made resident), barriers between dispatches (the resources are hazard tracked: see
// docs/backends-plan.md), and copies between buffers and images on its blit encoder
#include <rhi/metal/backend.h>

#include <rhi/buffer.h>
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

	ENSUREF(false, "the Metal backend dispatches by threads (DispatchThreads) so far: its thread group size is the pipeline's");
	(void)groupCountX;
	(void)groupCountY;
	(void)groupCountZ;
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
