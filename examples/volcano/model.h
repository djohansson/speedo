#pragma once

#include "aabb.h"
#include "buffer.h"
#include "device.h"
#include "utils.h"
#include "vertex.h"

#include <filesystem>
#include <string>
#include <vector>


template <GraphicsBackend B>
struct ModelDesc
{
	std::shared_ptr<DeviceContext<B>> deviceContext;
	AABB3f aabb = {};
	// todo: reconsider.
	std::vector<SerializableVertexInputAttributeDescription<B>> attributes;
	//
	DeviceSize<B> vertexBufferSize = 0;
	DeviceSize<B> indexBufferSize = 0;
	uint32_t indexCount = 0;
	// temp: these will be destroyed when calling deleteInitialData()
	BufferHandle<B> initialVertices = 0;
	AllocationHandle<B> initialVerticesMemory = 0;
	BufferHandle<B> initialIndices = 0;
	AllocationHandle<B> initialIndicesMemory = 0;
    //
	// todo: reconsider.
	std::string debugName;
	//
};

template <GraphicsBackend B>
class Model : Noncopyable
{
public:

	Model(ModelDesc<B>&& desc, CommandContext<B>& commandContext);
	Model(const std::filesystem::path& modelFile, CommandContext<B>& commandContext);

	const auto& getModelDesc() const { return myModelDesc; }
	const auto& getVertexBuffer() const { return myVertexBuffer; }
	const auto& getIndexBuffer() const { return myIndexBuffer; }
	const auto& getBindings() const { return myBindings; }

private:

	void deleteInitialData();

	ModelDesc<B> myModelDesc = {};
	std::vector<VertexInputBindingDescription<B>> myBindings;
	// todo: make one buffer + offsets for all model data
	Buffer<B> myVertexBuffer = {};
	Buffer<B> myIndexBuffer = {};
	//
};
