#pragma once

#include <rhi/types.h>

#include <cstddef>
#include <cstdint>
#include <span>

namespace rhi
{

template <GraphicsApi G>
class Buffer;

template <GraphicsApi G>
class Image;

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
	void BindIndexBuffer(const Buffer<G>& buffer, uint64_t offset, IndexType type) const;
	void DrawIndexed(uint32_t indexCount, uint32_t instanceCount = 1, uint32_t firstIndex = 0, int32_t vertexOffset = 0, uint32_t firstInstance = 0) const;
	void Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) const;
	// writes data (at most 64 kB, a multiple of 4 bytes) to buffer at offset, in order with the other commands
	void UpdateBuffer(const Buffer<G>& buffer, uint64_t offset, std::span<const std::byte> data) const;
	// writes value to every 4 bytes of size bytes of buffer from offset (size 0: to its end). outside of render passes,
	// on a buffer with BufferUsage::kTransferDestination
	void FillBuffer(const Buffer<G>& buffer, uint64_t offset, uint64_t size, uint32_t value) const;
	// makes the memory accesses (srcAccess) of the commands before, in srcStages, visible to the accesses (dstAccess) of
	// the commands after, in dstStages, which wait for them
	void Barrier(PipelineStage srcStages, Access srcAccess, PipelineStage dstStages, Access dstAccess) const;

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
