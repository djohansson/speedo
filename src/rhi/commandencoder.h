#pragma once

#include <rhi/shaderset.h>
#include <rhi/types.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace rhi
{

template <GraphicsApi G>
class Buffer;

template <GraphicsApi G>
class Image;

template <GraphicsApi G>
class CommandBuffer;

// records commands into a command buffer, in rhi's neutral terms. it doesn't own the command buffer: construct one
// wherever a command buffer is at hand.
template <GraphicsApi G>
class CommandEncoder final
{
public:
	explicit CommandEncoder(CommandBufferHandle<G> cmd) noexcept : myCmd(cmd) {}

	void SetViewport(const Viewport& viewport) const;
	void SetScissor(const Rect& rect) const;
	// which faces draws discard: dynamic state, so it must be set before drawing with the pipeline
	void SetCullMode(CullMode mode) const;
	void SetFrontFace(FrontFace face) const;
	// the value draws that write depth write to the stencil's first bit too (both faces), where the target has one: e.g.
	// to mark what moves (see the depth reprojection)
	void SetStencilReference(uint32_t reference) const;
	void BindIndexBuffer(const Buffer<G>& buffer, uint64_t offset, IndexType type) const;
	void DrawIndexed(uint32_t indexCount, uint32_t instanceCount = 1, uint32_t firstIndex = 0, int32_t vertexOffset = 0, uint32_t firstInstance = 0) const;
	// drawCount indexed draws, each from a command in buffer (BufferUsage::kIndirect: index count, instance count, first
	// index, vertex offset and first instance, as uint32s but the offset, an int32) from offset, stride bytes apart. the
	// vertex stage tells them apart by its draw index (SV_DrawIndex), 0 to drawCount - 1
	void DrawIndexedIndirect(const Buffer<G>& buffer, uint64_t offset, uint32_t drawCount, uint32_t stride) const;
	// the same, with the draw count read from countBuffer at countOffset (a uint32, at most maxDrawCount)
	void DrawIndexedIndirectCount(
		const Buffer<G>& buffer, uint64_t offset, const Buffer<G>& countBuffer, uint64_t countOffset, uint32_t maxDrawCount, uint32_t stride) const;
	void Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) const;
	// dispatches the bound compute pipeline (whose thread group size launch is, see Pipeline::GetComputeLaunchParameters)
	// over at least this many threads, in whole groups
	void DispatchThreads(const ComputeLaunchParameters& launch, uint32_t threadsX, uint32_t threadsY = 1, uint32_t threadsZ = 1) const;
	// writes data (at most 64 kB, a multiple of 4 bytes) to buffer at offset, in order with the other commands
	void UpdateBuffer(const Buffer<G>& buffer, uint64_t offset, std::span<const std::byte> data) const;
	// writes value to every 4 bytes of size bytes of buffer from offset (size 0: to its end). outside of render passes,
	// on a buffer with BufferUsage::kTransferDestination
	void FillBuffer(const Buffer<G>& buffer, uint64_t offset, uint64_t size, uint32_t value) const;
	// copies a color image's first mip level, in kTransferSource, into buffer (with BufferUsage::kTransferDestination),
	// tightly packed rows from its start. outside of render passes
	void CopyImageToBuffer(const Image<G>& image, const Buffer<G>& buffer) const;
	// size bytes of source to destination, from offset in each (the start by default)
	void CopyBuffer(const Buffer<G>& source, const Buffer<G>& destination, uint64_t size, uint64_t offset = 0) const;
	// an image this has only the handle of (e.g. a swapchain's), of extent, in kTransferSource
	void CopyImageToBuffer(ImageHandle<G> image, Extent2d extent, const Buffer<G>& buffer) const;
	// makes the memory accesses (srcAccess) of the commands before, in srcStages, visible to the accesses (dstAccess) of
	// the commands after, in dstStages, which wait for them
	void Barrier(PipelineStage srcStages, Access srcAccess, PipelineStage dstStages, Access dstAccess) const;
	// runs secondary command buffers (see CommandPool::BeginSecondary), inside the render target they were begun for:
	// takes them, each ended if it is still recording and handed back to its pool
	void ExecuteCommands(std::vector<CommandBuffer<G>>&& secondaries) const;

	// queue family ownership transfer, for a resource written on a queue of one family (srcFamily) and then used on a
	// queue of another (dstFamily): resources are exclusive to one family at a time. record the release on the source
	// queue after the writes (in srcStages, with srcAccess), and the same transfer's acquire on the destination queue
	// before the uses (in dstStages, with dstAccess), in a submission that waits for the release's. an image keeps its
	// layout. both are no-ops if the families are the same.
	void ReleaseOwnership(const Buffer<G>& buffer, uint32_t srcFamily, uint32_t dstFamily, PipelineStage srcStages, Access srcAccess) const;
	void AcquireOwnership(const Buffer<G>& buffer, uint32_t srcFamily, uint32_t dstFamily, PipelineStage dstStages, Access dstAccess) const;
	void ReleaseOwnership(const Image<G>& image, uint32_t srcFamily, uint32_t dstFamily, PipelineStage srcStages, Access srcAccess) const;
	void AcquireOwnership(const Image<G>& image, uint32_t srcFamily, uint32_t dstFamily, PipelineStage dstStages, Access dstAccess) const;

	[[nodiscard]] CommandBufferHandle<G> GetHandle() const noexcept { return myCmd; }

private:
	CommandBufferHandle<G> myCmd{};
};

} // namespace rhi
