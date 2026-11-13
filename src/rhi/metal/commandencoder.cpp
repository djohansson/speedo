// the Metal backend's commands: a dispatch on the command buffer's compute encoder, with what the pipeline bound for it
// (the pipeline state, the push constants at buffer 0, each set's argument buffer at its index, and what the sets
// reference made resident), and barriers between dispatches (the resources are hazard tracked: see
// docs/backends-plan.md)
#include <rhi/metal/backend.h>

#include <rhi/commandencoder.h>

#include <core/assert.h>
#include <core/profiling.h>

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
		encoder->setBytes(cmd.pushConstants.data(), cmd.pushConstants.size(), 0);
	for (NS::UInteger set = 0; set < cmd.sets.size(); set++)
	{
		const auto& binding = cmd.sets[set];
		if (binding.argumentBuffer == nullptr)
			continue;
		encoder->setBuffer(binding.argumentBuffer, binding.offset, set);
		for (auto* resource : binding.resources)
			encoder->useResource(resource, MTL::ResourceUsageRead | MTL::ResourceUsageWrite);
	}
	return encoder;
}

} // namespace commandencoder

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
