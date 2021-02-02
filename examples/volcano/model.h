#pragma once

#include "aabb.h"
#include "buffer.h"
#include "device.h"
#include "vertex.h"

#include <memory>
#include <tuple>
#include <vector>

template <GraphicsBackend B>
struct ModelCreateDesc : DeviceResourceCreateDesc<B>
{
	AABB3f aabb = {};
	DeviceSize<B> indexBufferSize = 0;
	DeviceSize<B> vertexBufferSize = 0;
	uint32_t indexCount = 0;
	std::vector<VertexInputAttributeDescription<B>> attributes;
};

template <GraphicsBackend B>
class Model : public Buffer<B>
{
public:

	constexpr Model() = default;
	Model(Model&& other) noexcept = default;
	Model( // loads a file into a buffer and creates a new model from it. buffer gets garbage collected when finished copying.
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        const std::shared_ptr<CommandContext<B>>& commandContext,
        const std::filesystem::path& modelFile);

	Model& operator=(Model&& other) noexcept = default;
	
	void swap(Model& rhs) noexcept;
	friend void swap(Model& lhs, Model& rhs) noexcept { lhs.swap(rhs); }

	const auto& getDesc() const { return myDesc; }
	const auto& getBindings() const { return myBindings; }
	constexpr auto getIndexOffset() const { return 0; }
	const auto& getVertexOffset() const { return myDesc.indexBufferSize; }

private:

	Model( // copies buffer in descAndInitialData into the target. descAndInitialData buffer gets automatically garbage collected when copy has finished.
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        const std::shared_ptr<CommandContext<B>>& commandContext,
        std::tuple<ModelCreateDesc<B>, BufferHandle<B>, AllocationHandle<B>>&& descAndInitialData);

	ModelCreateDesc<B> myDesc = {};
	std::vector<VertexInputBindingDescription<B>> myBindings;
};

#include "model.inl"
