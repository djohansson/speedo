#pragma once

// the Metal backend's own objects behind its opaque handles (rhi/metal/types.inl), for its sources alone
#include <rhi/types.h>

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <cstdint>
#include <vector>

namespace rhi::metal
{

// a command buffer: Metal's (one-shot, made from the pool's queue), the encoder open on it (one at a time: ended when
// the commands' kind changes, and at End), and what was bound for the next dispatch, which the compute encoder takes
// once it is open
struct CommandBufferObject
{
	MTL::CommandBuffer* commandBuffer = nullptr; // retained
	MTL::ComputeCommandEncoder* computeEncoder = nullptr; // retained while open
	MTL::BlitCommandEncoder* blitEncoder = nullptr; // retained while open
	MTL::ComputePipelineState* computePipeline = nullptr; // bound (the pipeline's, not retained)
	std::vector<uint8_t> pushConstants; // bound at buffer 0
	struct SetBinding
	{
		MTL::Buffer* argumentBuffer = nullptr; // the set's (the pipeline's, not retained)
		uint64_t offset = 0;
		std::vector<MTL::Resource*> resources; // what it references, made resident at the dispatch
	};
	std::vector<SetBinding> sets; // by set index, bound at buffer N

	void EndEncoders();
	MTL::ComputeCommandEncoder* ComputeEncoder();
	MTL::BlitCommandEncoder* BlitEncoder();
};

// a command pool: the queue its command buffers are made from and committed to
struct CommandPoolObject
{
	MTL::CommandQueue* queue = nullptr; // the queue's (not retained)
};

// a pipeline: a compute pipeline state (graphics ones come with the render passes, step 5)
struct PipelineObject
{
	MTL::ComputePipelineState* computePipeline = nullptr; // retained
};

// a descriptor set: an argument buffer (tier 2) of 8 byte entries, binding b at 8 times the counts before it (see
// docs/backends-plan.md), and what it references
struct DescriptorSetObject
{
	MTL::Buffer* argumentBuffer = nullptr; // retained, shared storage
	std::vector<MTL::Resource*> resources;
};

// a fence: a shared event and the value it waits for
struct FenceObject
{
	MTL::SharedEvent* event = nullptr; // retained
	uint64_t value = 0;
};

[[nodiscard]] inline MTL::Device* ToMetal(MTL::Device* device) noexcept { return device; }

} // namespace rhi::metal
