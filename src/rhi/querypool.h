#pragma once

#include <rhi/deviceobject.h>
#include <rhi/types.h>

#include <cstdint>
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
	// records writing a timestamp once the commands before have completed
	void WriteTimestamp(CommandBufferHandle<G> cmd, uint32_t index) const;
	// the timestamps from first, if all of them are available (doesn't wait)
	[[nodiscard]] bool Read(uint32_t first, std::span<uint64_t> timestamps) const;

private:
	QueryPool(CreateDescType&& desc, QueryPoolHandle<G>&& pool);

	QueryPoolHandle<G> myPool{};
};

} // namespace rhi
