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
		std::vector<uint8_t> pushed; // a pushed set's argument buffer (Pipeline::InternalPushDescriptorSet), set by setBytes
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

// a descriptor set layout's flag (DescriptorSetLayoutCreateDesc::flags): its descriptors pushed at each bind
inline constexpr uint32_t kDescriptorSetLayoutPush = 1U;

// a descriptor set layout: an argument buffer's (tier 2), of 8 byte entries (a buffer's gpu address, a texture's,
// sampler's or acceleration structure's resource id), binding b at 8 times the counts of the bindings before it, as
// slang lays out a parameter block's fields in their declaration order (see docs/backends-plan.md)
struct DescriptorSetLayoutObject
{
	static constexpr uint32_t kEntrySize = 8;

	struct Binding
	{
		uint32_t binding = 0;
		rhi::DescriptorType type{};
		uint32_t count = 0;
		uint32_t offset = 0; // in bytes, in the argument buffer
	};
	std::vector<Binding> bindings; // by binding
	uint32_t size = 0; // the argument buffer's, in bytes
	bool push = false;

	[[nodiscard]] const Binding& Find(uint32_t binding) const;
};

// a descriptor set: its part of an argument buffer (each array of sets is a buffer of its own, see DescriptorSetArray),
// and what it references, which a dispatch makes resident
struct DescriptorSetObject
{
	MTL::Buffer* argumentBuffer = nullptr; // retained, shared storage
	uint64_t offset = 0;
	std::vector<MTL::Resource*> resources;
};

// a pipeline layout: no object in Metal (a pipeline state has its sets at their buffer indices, the push constants at
// buffer 0): what the layout's handle tells apart
struct PipelineLayoutObject
{
	uint32_t pushConstantSize = 0;
};

// an image view: a texture view of the image's texture (a format, its levels, a swizzle)
struct ImageViewObject
{
	MTL::Texture* texture = nullptr; // retained
};

// rhi's format as Metal's (image.cpp): fails hard for one Metal hasn't (24 bit color, depth 24 stencil 8 on Apple's
// gpus)
[[nodiscard]] MTL::PixelFormat ToMetal(Format format);

// a fence: a shared event and the value it waits for
struct FenceObject
{
	MTL::SharedEvent* event = nullptr; // retained
	uint64_t value = 0;
};

[[nodiscard]] inline MTL::Device* ToMetal(MTL::Device* device) noexcept { return device; }

} // namespace rhi::metal
