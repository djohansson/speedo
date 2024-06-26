#pragma once

#include "buffer.h"
#include "device.h"

#include <gfx/aabb.h>
#include <gfx/vertex.h>

#include <memory>
#include <tuple>
#include <vector>

template <GraphicsApi G>
struct ModelCreateDesc
{
	AABB3f aabb;
	uint32_t indexCount = 0;
	uint32_t vertexCount = 0;
	std::vector<VertexInputAttributeDescription<G>> attributes;
};

template <GraphicsApi G>
class Model final
{
public:
	constexpr Model() noexcept = default;
	Model(Model&& other) noexcept = default;
	Model( // loads a file into a buffer and creates a new model from it. buffer gets garbage collected when finished copying.
		const std::shared_ptr<Device<G>>& device,
		Queue<G>& queue,
		uint64_t timelineValue,
		const std::filesystem::path& modelFile,
		std::atomic_uint8_t& progress);

	Model& operator=(Model&& other) noexcept = default;

	void Swap(Model& rhs) noexcept;
	friend void Swap(Model& lhs, Model& rhs) noexcept { lhs.Swap(rhs); }

	[[nodiscard]] const auto& GetDesc() const noexcept { return myDesc; }
	[[nodiscard]] const auto& GetBindings() const noexcept { return myBindings; }
	[[nodiscard]] const auto& GetIndexBuffer() { return myIndexBuffer; }
	[[nodiscard]] const auto& GetVertexBuffer() { return myVertexBuffer; }
	
private:
	Model( // copies buffer in descAndInitialData into the target. descAndInitialData buffer gets automatically garbage collected when copy has finished.
		const std::shared_ptr<Device<G>>& device,
		Queue<G>& queue,
		uint64_t timelineValue,
		std::tuple<
			ModelCreateDesc<G>,
			BufferHandle<G>,
			AllocationHandle<G>,
			BufferHandle<G>,
			AllocationHandle<G>>&& descAndInitialData);

	ModelCreateDesc<G> myDesc{};
	Buffer<kVk> myIndexBuffer;
	Buffer<kVk> myVertexBuffer;
	std::vector<VertexInputBindingDescription<G>> myBindings;
};
