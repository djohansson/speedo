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
	DeviceSize<G> indexBufferSize = 0;
	DeviceSize<G> vertexBufferSize = 0;
	uint32_t indexCount = 0;
	std::vector<VertexInputAttributeDescription<G>> attributes;
};

template <GraphicsApi G>
class Model final : public Buffer<G>
{
public:
	constexpr Model() noexcept = default;
	Model(Model&& other) noexcept = default;
	Model( // loads a file into a buffer and creates a new model from it. buffer gets garbage collected when finished copying.
		const std::shared_ptr<Device<G>>& device,
		QueueContext<G>& queueContext,
		const std::filesystem::path& modelFile);

	Model& operator=(Model&& other) noexcept = default;

	void swap(Model& rhs) noexcept;
	friend void swap(Model& lhs, Model& rhs) noexcept { lhs.swap(rhs); }

	const auto& getDesc() const noexcept { return myDesc; }
	const auto& getBindings() const noexcept { return myBindings; }
	static constexpr auto getIndexOffset() { return 0; }
	const auto& getVertexOffset() const noexcept { return myDesc.indexBufferSize; }

private:
	Model( // copies buffer in descAndInitialData into the target. descAndInitialData buffer gets automatically garbage collected when copy has finished.
		const std::shared_ptr<Device<G>>& device,
		QueueContext<G>& queueContext,
		std::tuple<ModelCreateDesc<G>, BufferHandle<G>, AllocationHandle<G>>&& descAndInitialData);

	ModelCreateDesc<G> myDesc{};
	std::vector<VertexInputBindingDescription<G>> myBindings;
};
