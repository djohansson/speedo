#pragma once

#include <gfx/gpu.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

// a frame's gpu work as a graph of passes over the resources they use, which records the barriers and layout
// transitions between them, and owns the resources that only live within a frame (transients), placed in shared memory
// where their lifetimes don't overlap.
//
// passes are added once, in the order they run, each declaring what it does with which resource (ResourceUse). Compile
// culls the passes nothing needs, creates the transients (sized by the frame's extent, recreated when it changes:
// call it with the gpu idle, as on a resize) and places them; Execute records the enabled passes into a command
// buffer, each after the barriers its uses need, given how the resources were last used (tracked across frames, and
// across the transients sharing memory). raster passes get their attachments begun as a render target.
namespace gfx
{

// what a pass does with a resource: for an image, the layout it must be in; and the pipeline stages that access it
// with which accesses (whether it writes decides what waits for what)
struct ResourceUse
{
	rhi::ImageLayout layout = rhi::ImageLayout::kUndefined;
	rhi::PipelineStage stages = rhi::PipelineStage::kNone;
	rhi::Access access = rhi::Access::kNone;

	[[nodiscard]] bool Writes() const noexcept;

	[[nodiscard]] static ResourceUse ColorAttachment();
	[[nodiscard]] static ResourceUse DepthAttachment();
	[[nodiscard]] static ResourceUse Sampled(rhi::PipelineStage stages);
	// a storage image (in kGeneral) or buffer, read and or written by shaders in stages
	[[nodiscard]] static ResourceUse Storage(rhi::PipelineStage stages, rhi::Access access);
	[[nodiscard]] static ResourceUse TransferSource();
	[[nodiscard]] static ResourceUse TransferDestination();
	[[nodiscard]] static ResourceUse HostRead();
	[[nodiscard]] static ResourceUse Present();
};

class FrameGraph final
{
public:
	struct ImageId
	{
		uint32_t index = UINT32_MAX;
		[[nodiscard]] explicit operator bool() const noexcept { return index != UINT32_MAX; }
	};
	struct BufferId
	{
		uint32_t index = UINT32_MAX;
		[[nodiscard]] explicit operator bool() const noexcept { return index != UINT32_MAX; }
	};
	struct PassId
	{
		uint32_t index = UINT32_MAX;
	};

	// a transient image: 2d, of the frame's extent unless extent is set, with a full mip chain if mipChain. its usage
	// follows from what the passes do with it, and its contents don't outlive the frame: its first use in a frame must
	// write all of it (a cleared attachment, a transfer)
	struct ImageDesc
	{
		std::string name;
		rhi::Format format = rhi::Format::kUndefined;
		rhi::ImageAspect aspect = rhi::ImageAspect::kColor;
		rhi::Extent2d extent{};
		bool mipChain = false;
	};
	// a transient buffer of size bytes (computed from the frame's extent if sizeOf is set)
	struct BufferDesc
	{
		std::string name;
		uint64_t size = 0;
		std::function<uint64_t(rhi::Extent2d extent)> sizeOf;
	};

	// what a pass's execute callback records with
	struct PassContext
	{
		FrameGraph& graph;
		CommandBufferHandle cmd{};
		// a raster pass's render target, begun (with the contents its builder asked for), and how
		IRenderTarget* target = nullptr;
		const RenderTargetBeginInfo* beginInfo = nullptr;
	};
	using ExecuteFunction = std::function<void(PassContext& context)>;
	// records around a pass: calls record (the pass's barriers, render target and execute callback) in between
	using WrapFunction = std::function<void(CommandBufferHandle cmd, const std::function<void()>& record)>;

	// declares what a pass uses (called once, from AddPass)
	class PassBuilder
	{
	public:
		void Use(ImageId image, const ResourceUse& use);
		void Use(BufferId buffer, const ResourceUse& use);
		// makes the pass a raster pass over these attachments (all transient, or all of one bound render target), each
		// in its attachment layout, loaded (or cleared, or not) and stored as given
		void ColorAttachment(
			ImageId image,
			rhi::LoadOp load,
			rhi::StoreOp store = rhi::StoreOp::kStore,
			const rhi::ClearValue& clear = {});
		void DepthAttachment(
			ImageId image,
			rhi::LoadOp load,
			rhi::StoreOp store = rhi::StoreOp::kStore,
			const rhi::ClearValue& clear = {.depth = 1.0F, .stencil = 0});
		// how a raster pass records its commands: inline (default) or in secondary command buffers
		void Contents(rhi::SubpassContents contents);
		// keeps the pass even if nothing in the graph reads what it writes (writes to imported resources do that already)
		void SideEffects();
		// records commands around the pass, outside its render target: e.g. a gpu profiling scope, which a render target
		// begun for secondary command buffers can't hold
		void Wrap(WrapFunction wrap);

	private:
		friend class FrameGraph;
		PassBuilder(FrameGraph& graph, uint32_t pass) : myGraph(graph), myPass(pass) {}

		FrameGraph& myGraph;
		uint32_t myPass;
	};

	FrameGraph() = default;
	FrameGraph(const FrameGraph&) = delete;
	FrameGraph& operator=(const FrameGraph&) = delete;
	~FrameGraph();

	[[nodiscard]] ImageId CreateImage(ImageDesc desc);
	[[nodiscard]] BufferId CreateBuffer(BufferDesc desc);
	// images and buffers made elsewhere, bound before each Execute (see Bind): their contents persist, and they are
	// outputs (passes writing them are never culled)
	[[nodiscard]] ImageId ImportImage(std::string name, rhi::ImageAspect aspect = rhi::ImageAspect::kColor);
	[[nodiscard]] BufferId ImportBuffer(std::string name);

	PassId AddPass(std::string name, const std::function<void(PassBuilder& builder)>& setup, ExecuteFunction execute);
	// a disabled pass is skipped by Execute, with nothing else changed (default: enabled)
	void SetEnabled(PassId pass, bool enabled);

	// culls, and (re)creates the transients for extent if it changed or they don't exist yet. the gpu must be done with
	// the previous transients. returns whether it (re)created them, after which their views and buffers have changed.
	bool Compile(Device& device, rhi::Extent2d extent);

	// binds an imported image: as an attachment of a render target (e.g. the swapchain's current frame), or alone
	void Bind(ImageId image, IRenderTarget& target, uint32_t index);
	void Bind(ImageId image, Image& bound);
	void Bind(BufferId buffer, Buffer& bound);

	// records the enabled passes, after their barriers
	void Execute(CommandBufferHandle cmd);

	[[nodiscard]] Image& GetImage(ImageId image) const;
	[[nodiscard]] const ImageView& GetView(ImageId image) const; // all of a sampled or storage transient's mips
	[[nodiscard]] Buffer& GetBuffer(BufferId buffer) const;
	[[nodiscard]] rhi::Extent2d GetExtent() const noexcept { return myExtent; }
	// the bytes of device memory the transients take, and how many they would take without sharing it
	[[nodiscard]] uint64_t GetTransientMemorySize() const noexcept { return myTransientMemorySize; }
	[[nodiscard]] uint64_t GetUnaliasedTransientMemorySize() const noexcept { return myUnaliasedTransientMemorySize; }

private:
	// how a resource was last used: the last write, and the reads since, which the next write waits for
	struct AccessState
	{
		rhi::PipelineStage writeStages = rhi::PipelineStage::kNone;
		rhi::Access writeAccess = rhi::Access::kNone;
		rhi::PipelineStage readStages = rhi::PipelineStage::kNone;
		rhi::Access readAccess = rhi::Access::kNone;
	};
	struct Resource
	{
		std::string name;
		bool image = true;
		bool imported = false;
		ImageDesc imageDesc;
		BufferDesc bufferDesc;
		rhi::ImageUsage imageUsage{};
		rhi::BufferUsage bufferUsage{};
		// created (transient) or bound (imported)
		std::shared_ptr<Image> transientImage;
		std::unique_ptr<ImageView> transientView;
		std::unique_ptr<Buffer> transientBuffer;
		std::variant<std::monostate, std::pair<IRenderTarget*, uint32_t>, Image*> boundImage;
		Buffer* boundBuffer = nullptr;
		// placement: the memory block, the bytes it takes there, and the transients over the same bytes (itself included)
		uint32_t block = UINT32_MAX;
		uint64_t offset = 0;
		uint64_t size = 0;
		std::vector<uint32_t> overlaps;
		// the live passes from the first to the last that use it
		uint32_t firstPass = UINT32_MAX;
		uint32_t lastPass = 0;
		AccessState state;
		bool usedThisFrame = false;
	};
	struct Attachment
	{
		uint32_t resource;
		rhi::LoadOp load;
		rhi::StoreOp store;
		rhi::ClearValue clear;
	};
	struct Pass
	{
		std::string name;
		std::vector<std::pair<uint32_t, ResourceUse>> uses;
		std::vector<Attachment> attachments; // color first, then depth
		rhi::SubpassContents contents = rhi::SubpassContents::kInline;
		ExecuteFunction execute;
		WrapFunction wrap;
		bool sideEffects = false;
		bool enabled = true;
		bool live = true;
		// a raster pass over transients: its render target (shared by the passes with the same attachments)
		std::shared_ptr<RenderImageSet> renderTarget;
	};

	void InternalCull();
	void InternalCreateTransients(Device& device);
	void InternalBarriers(CommandBufferHandle cmd, Pass& pass, uint32_t passIndex);
	void InternalRecord(CommandBufferHandle cmd, Pass& pass, uint32_t passIndex);
	void InternalTransition(CommandBufferHandle cmd, Resource& resource, rhi::ImageLayout layout);
	[[nodiscard]] rhi::ImageLayout InternalLayout(const Resource& resource) const;

	std::vector<Resource> myResources;
	std::vector<Pass> myPasses;
	std::vector<MemoryBlock> myBlocks;
	rhi::Extent2d myExtent{};
	bool myCompiled = false;
	uint64_t myTransientMemorySize = 0;
	uint64_t myUnaliasedTransientMemorySize = 0;
};

} // namespace gfx
