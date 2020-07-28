#pragma once

#include "aabb.h"
#include "buffer.h"
#include "device.h"
#include "vertex.h"


template <GraphicsBackend B>
struct ModelCreateDesc : DeviceResourceCreateDesc<B>
{
	AABB3f aabb = {};
	DeviceSize<B> indexBufferSize = 0;
	DeviceSize<B> vertexBufferSize = 0;
	uint32_t indexCount = 0;
	// todo: reconsider.
	std::vector<SerializableVertexInputAttributeDescription<B>> attributes;
	//
};

template <GraphicsBackend B>
class Model
{
public:

	Model(Model&& other) = default;
	Model( // copies the initial buffer into a new one. buffer gets garbage collected when finished copying.
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        const std::shared_ptr<CommandContext<B>>& commandContext,
        std::tuple<ModelCreateDesc<B>, BufferHandle<B>, AllocationHandle<B>>&& descAndInitialData);
	Model( // loads a file into a buffer and creates a new model from it. buffer gets garbage collected when finished copying.
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        const std::shared_ptr<CommandContext<B>>& commandContext,
        const std::filesystem::path& modelFile);

	const auto& getDesc() const { return myDesc; }
	const auto& getBuffer() const { return myBuffer; }
	const auto& getBindings() const { return myBindings; }
	constexpr auto getIndexOffset() const { return 0; }
	const auto& getVertexOffset() const { return myDesc.indexBufferSize; }

private:

	const ModelCreateDesc<B> myDesc = {};
	Buffer<B> myBuffer;
	std::vector<VertexInputBindingDescription<B>> myBindings;
};
