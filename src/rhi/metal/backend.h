#pragma once

// the Metal backend's own objects behind its opaque handles (rhi/metal/types.inl), for its sources alone
#include <rhi/shaderset.h>
#include <rhi/types.h>

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <optional>
#include <string_view>
#include <utility>
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
	MTL::AccelerationStructureCommandEncoder* accelerationStructureEncoder = nullptr; // retained while open
	MTL::RenderCommandEncoder* renderEncoder = nullptr; // retained while open (a render target's pass, Begin to End)
	// a render target's pass begun, its encoder made at its first command (RenderEncoder, or EndEncoders for a pass of
	// clears alone), so that a timestamp written just inside the pass samples where it starts (retained until then)
	MTL::RenderPassDescriptor* pendingRenderPass = nullptr;
	// the timestamps' samples (see Timestamp): the next encoder's, the open one's, and the one just ended's (none begun
	// since)
	std::optional<TimestampSamples> pendingSamples;
	std::optional<TimestampSamples> openSamples;
	std::optional<TimestampSamples> lastSamples;
	// the passes being recorded (CrashBreadcrumb's: the frame graph's), innermost last, so that a failed command buffer's
	// report names them (each encoder's label and debug signposts, see commandpool::ReportErrors): an encoder made
	// meanwhile is labelled by them, and one open when a pass begins takes its name as a debug group, popped at the pass's
	// end or before the encoder ends, whichever is first (Metal's debug groups must balance within an encoder)
	struct Breadcrumb
	{
		std::string name;
		MTL::CommandEncoder* encoder = nullptr; // the one it was pushed on, while it's open (not retained)
	};
	std::vector<Breadcrumb> breadcrumbs;
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
		bool accelerationStructures = false; // it references a top level structure: the bottom levels made resident too
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
	MTL::AccelerationStructureCommandEncoder* AccelerationStructureEncoder();
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
	// the encoder open (none, or one of the four)
	[[nodiscard]] MTL::CommandEncoder* OpenEncoder() const;
	// an encoder begun now labelled by the passes being recorded (see breadcrumbs)
	void LabelEncoder(MTL::CommandEncoder* encoder) const;
	// a pass's breadcrumb begun and ended (CrashBreadcrumb; a secondary's are its primary's)
	void BeginBreadcrumb(std::string_view name);
	void EndBreadcrumb();
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
	std::vector<MTL::CounterSampleBuffer*> buffers; // retained (a pool of timestamps')
	MTL::Buffer* sizes = nullptr; // a pool of acceleration structures' compacted sizes': 8 bytes a query (retained, shared)
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
	bool accelerationStructures = false; // see CommandBufferObject::SetBinding
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

	// the presents' ids (from 1, Queue::Present's) and the last a drawable's presented handler reported (shown or
	// dropped: the frame is off the display's queue either way), which Swapchain::WaitPresent waits for. shared with the
	// handlers, which may run after the swapchain is gone
	struct Presents
	{
		std::mutex mutex;
		std::condition_variable presentedChanged;
		uint64_t submitted = 0;
		uint64_t presented = 0;
	};
	std::shared_ptr<Presents> presents = std::make_shared<Presents>();
};

// the device's allocator: the device heaps are made from (no allocator of Metal's own: a resource not placed in a memory
// block is the device's)
struct AllocatorObject
{
	MTL::Device* device = nullptr; // the device's (not retained)
	// what top level builds convert their instances with (accelerationstructure.cpp), made at the first (retained)
	std::mutex mutex;
	MTL::ComputePipelineState* instancePipeline = nullptr;
};

// the buffers by their gpu addresses (every Buffer<kMetal>'s while it lives), for what rhi names by address (an
// acceleration structure build's inputs and scratch memory) as Metal takes it: a buffer and an offset in it
void RegisterBuffer(MTL::Buffer* buffer);
void UnregisterBuffer(MTL::Buffer* buffer) noexcept;
[[nodiscard]] std::pair<MTL::Buffer*, uint64_t> FindBuffer(uint64_t address);

// the live bottom level structures declared used on an encoder (read): a top level structure references them by
// resource id, which Metal neither makes resident nor tracks hazards through (accelerationstructure.cpp)
void UseBottomLevels(MTL::ComputeCommandEncoder* encoder);
void UseBottomLevels(MTL::RenderCommandEncoder* encoder);
void UseBottomLevels(MTL::AccelerationStructureCommandEncoder* encoder);

// whether the application asks for ray queries: opt in (SPEEDO_RAY_QUERY=1), as Vulkan's backend's on macOS
// (rhi::vk::RayQueriesEnabled: Apple's gpu restarts with ray tracing beside other gpu work)
[[nodiscard]] bool RayQueriesEnabled();

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

// a shader module: its entry point's library, compiled by Metal's runtime compiler asynchronously from the module's
// creation (a layout's dozens of entry points compile in parallel, on Metal's threads), waited for by the first pipeline
// made of it, where the pipelines are made (the executor's tasks). compiled at the layout's creation, on the draw thread
// (Scene's first frames), a layout's libraries held the draw thread one after another: past the client's 20 s hang
// check in the quick tier's first batch of clients (3 of 61 lost, twice)
struct ShaderModuleObject
{
	std::string name; // the entry point's
	std::atomic<bool> done{false}; // the compile's ended (library or error set)
	MTL::Library* library = nullptr; // retained, once done (none where the compile failed)
	std::string error; // the compile's, where it failed

	// waits for the compile (Metal's, not a task's), failing hard where it failed
	[[nodiscard]] MTL::Library* Library();
};

// a render target's draws of its own: its clears of a rect inside its pass (RenderTarget::ClearRect: Metal clears no part
// of an attachment, so a triangle drawn over the rect, writing the clear value to the attachment alone, its depth and
// stencil for a depth stencil one) and its scaled blits (RenderTarget::Blit: a pass of its own on the destination, a
// triangle over it sampling the source). the library at the first, a pipeline per attachment at its first (rendertarget.cpp)
struct RenderTargetClearObject
{
	std::mutex mutex; // the first clears' and blits' (a target's passes may record on several threads)
	MTL::Library* library = nullptr; // retained
	std::vector<MTL::RenderPipelineState*> pipelines; // the clears', by attachment (retained, null until its first clear)
	std::vector<MTL::RenderPipelineState*> blitPipelines; // by destination attachment (retained, null until its first blit)
	MTL::SamplerState* nearest = nullptr; // a blit's filters (retained, clamped to the source's edge)
	MTL::SamplerState* linear = nullptr;
	MTL::DepthStencilState* keep = nullptr; // a color clear's: depth and stencil kept (retained)
	MTL::DepthStencilState* write = nullptr; // a depth clear's: depth written (retained)
	MTL::DepthStencilState* writeStencil = nullptr; // a depth stencil clear's: depth written, the stencil replaced (retained)

	RenderTargetClearObject() noexcept = default;
	RenderTargetClearObject(const RenderTargetClearObject&) = delete;
	RenderTargetClearObject& operator=(const RenderTargetClearObject&) = delete;
	~RenderTargetClearObject();
};

[[nodiscard]] inline MTL::Device* ToMetal(MTL::Device* device) noexcept { return device; }

} // namespace rhi::metal
