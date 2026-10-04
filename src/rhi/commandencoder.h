#pragma once

#include <rhi/types.h>

#include <cstddef>
#include <cstdint>
#include <span>

namespace rhi
{

template <GraphicsApi G>
class Buffer;

// records commands into a command buffer, in rhi's neutral terms. it doesn't own the command buffer: construct one
// wherever a command buffer is at hand.
template <GraphicsApi G>
class CommandEncoder final
{
public:
	explicit CommandEncoder(CommandBufferHandle<G> cmd) noexcept : myCmd(cmd) {}

	void SetViewport(const Viewport& viewport) const;
	void SetScissor(const Rect& rect) const;
	void BindIndexBuffer(const Buffer<G>& buffer, uint64_t offset, IndexType type) const;
	void DrawIndexed(uint32_t indexCount, uint32_t instanceCount = 1, uint32_t firstIndex = 0, int32_t vertexOffset = 0, uint32_t firstInstance = 0) const;
	void Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) const;
	// writes data (at most 64 kB, a multiple of 4 bytes) to buffer at offset, in order with the other commands
	void UpdateBuffer(const Buffer<G>& buffer, uint64_t offset, std::span<const std::byte> data) const;
	// makes the memory accesses (srcAccess) of the commands before, in srcStages, visible to the accesses (dstAccess) of
	// the commands after, in dstStages, which wait for them
	void Barrier(PipelineStage srcStages, Access srcAccess, PipelineStage dstStages, Access dstAccess) const;

	[[nodiscard]] CommandBufferHandle<G> GetHandle() const noexcept { return myCmd; }

private:
	CommandBufferHandle<G> myCmd{};
};

} // namespace rhi
