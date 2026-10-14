#pragma once

#include <rhi/deviceobject.h>
#include <rhi/enums.h>
#include <rhi/types.h>

#include <cstdint>
#include <optional>
#include <span>

namespace rhi
{

template <GraphicsApi G>
class QueryPool;

template <GraphicsApi G>
struct QueryPoolCreateDesc final : DeviceObjectCreateDesc<G>
{
	uint32_t count = 0;
};

template <GraphicsApi G>
struct ObjectTraits<QueryPool<G>>
{
	using CreateDescType = QueryPoolCreateDesc<G>;
};

// gpu timestamps: count of them, written by commands (in ticks of DeviceLimits::timestampPeriod nanoseconds), read
// back by the host once the commands are done
template <GraphicsApi G>
class QueryPool final : public DeviceObject<QueryPool<G>>
{
public:
	using SuperType = DeviceObject<QueryPool<G>>;
	using CreateDescType = ObjectTraits<QueryPool<G>>::CreateDescType;

	constexpr QueryPool() noexcept = default;
	explicit QueryPool(CreateDescType&& desc);
	QueryPool(QueryPool&& other) noexcept;
	~QueryPool();

	[[maybe_unused]] QueryPool& operator=(QueryPool&& other) noexcept;
	[[nodiscard]] operator auto() const noexcept { return myPool; } //NOLINT(google-explicit-constructor)

	void Swap(QueryPool& rhs) noexcept;
	friend void Swap(QueryPool& lhs, QueryPool& rhs) noexcept { lhs.Swap(rhs); }

	// records the reset of count timestamps from first (outside of render passes, before they are written again)
	void Reset(CommandBufferHandle<G> cmd, uint32_t first, uint32_t count) const;
	// resets count timestamps from first, from the host (once the gpu is done with them: Vulkan 1.2's host query reset)
	void Reset(uint32_t first, uint32_t count) const;
	// records writing a timestamp once the commands before have reached stage (bottom of pipe: completed). inside a render
	// pass with inline contents, top of pipe and bottom of pipe time the pass's own work: KosmicKrisp samples a render
	// encoder's stages there, where outside one it writes from a compute encoder, whose samples read 0 or stale once
	// the encoder holds other work (see FrameGraph::Execute)
	void WriteTimestamp(CommandBufferHandle<G> cmd, uint32_t index, PipelineStage stage = PipelineStage::kBottomOfPipe) const;
	// the timestamps from first, each if it was written since its reset (doesn't wait). a backend may leave some unwritten:
	// KosmicKrisp only writes them where its own Metal command encoders begin and end, and an unwritten one's value is
	// undefined (it read back as one written frames before)
	void Read(uint32_t first, std::span<std::optional<uint64_t>> timestamps) const;

private:
	QueryPool(CreateDescType&& desc, QueryPoolHandle<G>&& pool);

	QueryPoolHandle<G> myPool{};
};

} // namespace rhi
