#pragma once

// the Metal backend's own objects behind its opaque handles (rhi/metal/types.inl), for its sources alone
#include <rhi/shaderset.h>
#include <rhi/types.h>

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>

#include <array>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

namespace rhi::metal
{

struct QueryPoolObject;

// timestamps (QueryPool::WriteTimestamp): Apple's gpus sample counters only where an encoder begins or ends
// (MTLCounterSamplingPointAtStageBoundary), so a timestamp is one of those samples: an encoder sampled has a pair of them
// in a query pool's sample buffer, its start's and its end's
struct TimestampSamples
{
	MTL::CounterSampleBuffer* buffer = nullptr; // the pool's
	NS::UInteger start = 0; // the encoder's start's (a compute or blit encoder's, a render pass's vertex stage's); its end's next
};
struct TimestampSample
{
	MTL::CounterSampleBuffer* buffer = nullptr;
	NS::UInteger index = 0;
};

// a command buffer: Metal's (one-shot, made from the pool's queue), the encoder open on it (one at a time: ended when
// the commands' kind changes, and at End), and what was bound for the next dispatch, which the compute encoder takes
// once it is open
struct CommandBufferObject
{
	MTL::CommandBuffer* commandBuffer = nullptr; // retained (none for a secondary)
	// a secondary's: the primary whose render pass it records in, into its open render encoder (Metal has no secondary
	// command buffers: what a secondary records is recorded inline, ExecuteCommands left nothing to do)
	CommandBufferObject* primary = nullptr;
	MTL::ComputeCommandEncoder* computeEncoder = nullptr; // retained while open
	MTL::BlitCommandEncoder* blitEncoder = nullptr; // retained while open
	MTL::RenderCommandEncoder* renderEncoder = nullptr; // retained while open (a render target's pass, Begin to End)
	// a render target's pass begun, its encoder made at its first command (RenderEncoder, or EndEncoders for a pass of
	// clears alone), so that a timestamp written just inside the pass samples where it starts (retained until then)
	MTL::RenderPassDescriptor* pendingRenderPass = nullptr;
	// the timestamps' samples (see Timestamp): the next encoder's, the open one's, and the one just ended's (none begun
	// since)
	std::optional<TimestampSamples> pendingSamples;
	std::optional<TimestampSamples> openSamples;
	std::optional<TimestampSamples> lastSamples;
	MTL::ComputePipelineState* computePipeline = nullptr; // bound (the pipeline's, not retained)
	std::array<uint64_t, 3> computeThreadGroupSize{1, 1, 1}; // the bound compute pipeline's (Dispatch's group size)
	MTL::RenderPipelineState* renderPipeline = nullptr; // bound (the pipeline's, not retained)
	MTL::DepthStencilState* depthStencil = nullptr; // the bound render pipeline's (the pipeline's, not retained)
	MTL::PrimitiveType primitiveType = MTL::PrimitiveTypeTriangle; // the bound render pipeline's
	struct IndexBinding
	{
		MTL::Buffer* buffer = nullptr; // bound (not retained)
		uint64_t offset = 0;
		bool wide = true; // 32 bit indices, else 16
	} indices;
	std::vector<uint8_t> pushConstants; // bound at pushConstantSlot (the layout's, see PipelineLayoutObject)
	uint32_t pushConstantSlot = 0;
	struct SetBinding
	{
		MTL::Buffer* argumentBuffer = nullptr; // the set's (the pipeline's, not retained)
		uint64_t offset = 0;
		std::vector<uint8_t> pushed; // a pushed set's argument buffer (Pipeline::InternalPushDescriptorSet), set by setBytes
		std::vector<MTL::Resource*> resources; // what it references, made resident at the dispatch
	};
	std::vector<SetBinding> sets; // by slot (the buffer index the layout gives the set, see PipelineLayoutObject)
	// the render encoder's state as CommandEncoder set it, which a clear inside the pass (RenderTarget::ClearRect, a draw)
	// sets again after its own: none until set in the pass (the encoder's defaults: the whole attachment)
	std::optional<MTL::Viewport> viewport;
	std::optional<MTL::ScissorRect> scissor;
	MTL::CullMode cullMode = MTL::CullModeNone;
	uint32_t stencilReference = 0;
	// what the commands read from but nothing else holds (CommandEncoder::UpdateBuffer's data): released with the
	// command buffer, once its commands are done (see commandpool::Free)
	std::vector<MTL::Buffer*> transients;

	void EndEncoders();
	MTL::ComputeCommandEncoder* ComputeEncoder();
	MTL::BlitCommandEncoder* BlitEncoder();
	// a render pass (a render target's Begin), open until its End: its encoder made at the first command that needs it
	// (RenderEncoder: a secondary's is its primary's)
	void BeginRenderPass(MTL::RenderPassDescriptor* descriptor);
	[[nodiscard]] MTL::RenderCommandEncoder* RenderEncoder();
	// where a timestamp written now is sampled: the end of the open encoder if it was begun with samples (a compute or
	// blit encoder ended there, so that the work after it is timed apart; a render pass's open to its End), else the start
	// of the next encoder (a pair of pool's samples for it), or the end of the one just ended if none was begun since.
	// none in a render pass whose encoder was begun without samples. a secondary's are its primary's
	[[nodiscard]] std::optional<TimestampSample> Timestamp(QueryPoolObject& pool);
	// the samples an encoder begun now takes (those a timestamp left pending)
	std::optional<TimestampSamples> BeginSamples();
};

// a query pool of timestamps: pairs of samples in counter sample buffers (of the device's timestamp counter set, at most
// kSampleCount samples each: Metal's 32 KB), a pair per encoder sampled, handed out in a ring of as many pairs as the
// pool has queries (a timestamp takes at most one pair, so the ring can't overtake a query not yet read before the
// queries' own ring does), and per query its sample and the command buffer it's in (done once that has completed)
struct QueryPoolObject
{
	static constexpr NS::UInteger kSampleCount = 4096;

	struct Query
	{
		MTL::CommandBuffer* commandBuffer = nullptr; // retained while written
		std::optional<TimestampSample> sample; // none: never sampled (reads 0)
	};

	std::mutex mutex; // the pairs' and the queries' (written while recording, read by the profiler's Collect)
	std::vector<MTL::CounterSampleBuffer*> buffers; // retained
	std::vector<Query> queries;
	uint64_t nextPair = 0;

	[[nodiscard]] TimestampSamples NewPair();
};

// a command pool: the queue its command buffers are made from and committed to
struct CommandPoolObject
{
	MTL::CommandQueue* queue = nullptr; // the queue's (not retained)
};

// a pipeline: a compute pipeline state, or a render pipeline state with its depth stencil state (Metal keeps them apart)
// and the primitives its draws make (Metal's draws name them)
struct PipelineObject
{
	MTL::ComputePipelineState* computePipeline = nullptr; // retained
	std::array<uint64_t, 3> threadGroupSize{1, 1, 1}; // a compute pipeline's entry point's (its numthreads)
	MTL::RenderPipelineState* renderPipeline = nullptr; // retained
	MTL::DepthStencilState* depthStencil = nullptr; // retained
	MTL::PrimitiveType primitiveType = MTL::PrimitiveTypeTriangle;
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

// a pipeline layout: no object in Metal, but where its code takes each set and the push constants: their slots, the
// buffer indices slang gave them (by declaration order, see rhi::kNoShaderSlot), the same for every entry point of the
// layout's shader set
struct PipelineLayoutObject
{
	uint32_t pushConstantSize = 0;
	uint32_t pushConstantSlot = 0;
	std::vector<uint32_t> setSlots; // by set index (kNoShaderSlot for a set the layout hasn't)

	[[nodiscard]] uint32_t SetSlot(uint32_t set) const;
};

// an image view: a texture view of the image's texture (a format, its levels, a swizzle)
struct ImageViewObject
{
	MTL::Texture* texture = nullptr; // retained
};

// rhi's format as Metal's (image.cpp): fails hard for one Metal hasn't (24 bit color, depth 24 stencil 8 on Apple's
// gpus)
[[nodiscard]] MTL::PixelFormat ToMetal(Format format);

// a swapchain: its window's CAMetalLayer, and its images, textures of its own (render target, storage and blit usage,
// as Vulkan's swapchain images have: a drawable's texture is no storage image), each copied into the layer's next
// drawable when it is presented (Queue::Present)
struct SwapchainObject
{
	CA::MetalLayer* layer = nullptr; // the swapchain's surface (not retained here)
	std::vector<MTL::Texture*> textures; // retained
};

// the device's allocator: the device heaps are made from (no allocator of Metal's own: a resource not placed in a memory
// block is the device's)
struct AllocatorObject
{
	MTL::Device* device = nullptr; // the device's (not retained)
};

// a memory block's allocation: a placement heap, resources placed at offsets in it (see MemoryBlock)
struct AllocationObject
{
	MTL::Heap* heap = nullptr; // retained
};

// a fence: a shared event and the value it waits for
struct FenceObject
{
	MTL::SharedEvent* event = nullptr; // retained
	uint64_t value = 0;
};

// a render target's clears of a rect inside its pass (RenderTarget::ClearRect): Metal clears no part of an attachment, so
// a triangle drawn over the rect, writing the clear value to the attachment alone (its depth and stencil for a depth
// stencil one): the library at the first clear, a pipeline per attachment at its first (rendertarget.cpp)
struct RenderTargetClearObject
{
	std::mutex mutex; // the first clears' (a target's passes may record on several threads)
	MTL::Library* library = nullptr; // retained
	std::vector<MTL::RenderPipelineState*> pipelines; // by attachment (retained, null until its first clear)
	MTL::DepthStencilState* keep = nullptr; // a color clear's: depth and stencil kept (retained)
	MTL::DepthStencilState* write = nullptr; // a depth clear's: depth written, the stencil replaced (retained)

	RenderTargetClearObject() noexcept = default;
	RenderTargetClearObject(const RenderTargetClearObject&) = delete;
	RenderTargetClearObject& operator=(const RenderTargetClearObject&) = delete;
	~RenderTargetClearObject();
};

[[nodiscard]] inline MTL::Device* ToMetal(MTL::Device* device) noexcept { return device; }

} // namespace rhi::metal
