#pragma once

#include "buffer.h"
#include "device.h"

#include <gfx/bounds.h>
#include <gfx/vertex.h>

#include <memory>
#include <tuple>
#include <vector>

template <GraphicsApi G>
struct ModelCreateDesc
{
	Bounds3f bounds;
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
		std::span<TaskCreateInfo<void>> timelineCallbacksOut,
		CommandBufferHandle<G> cmd,
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
	Model( // copies buffer in initialData into the target. initialData buffer gets automatically garbage collected when copy has finished.
		const std::shared_ptr<Device<G>>& device,
		std::span<TaskCreateInfo<void>> timelineCallbacksOut,
		CommandBufferHandle<G> cmd,
		std::tuple<
			BufferHandle<G>,
			AllocationHandle<G>,
			BufferHandle<G>,
			AllocationHandle<G>,
			ModelCreateDesc<G>>&& initialData);

	Buffer<kVk> myIndexBuffer;
	Buffer<kVk> myVertexBuffer;
	std::vector<VertexInputBindingDescription<G>> myBindings;
	ModelCreateDesc<G> myDesc{};
};
