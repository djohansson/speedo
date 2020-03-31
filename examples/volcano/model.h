#pragma once

#include "aabb.h"
#include "buffer.h"
#include "vertex.h"

#include <filesystem>
#include <string>
#include <vector>


template <GraphicsBackend B>
struct ModelCreateDesc
{
	AABB3f aabb = {};
	std::vector<SerializableVertexInputAttributeDescription<B>> attributes;
	DeviceSize<B> vertexBufferSize = 0;
	DeviceSize<B> indexBufferSize = 0;
	uint32_t indexCount = 0;
	// these will be destroyed in Buffer:s constructor
	BufferHandle<B> initialVertices = 0;
	AllocationHandle<B> initialVerticesMemory = 0;
	BufferHandle<B> initialIndices = 0;
	AllocationHandle<B> initialIndicesMemory = 0;
    //
	std::string debugName;
};

template <GraphicsBackend B>
class Model
{
public:

	Model(ModelCreateDesc<B>&& desc,
		DeviceHandle<B> device, CommandPoolHandle<B> commandPool, QueueHandle<B> queue, AllocatorHandle<B> allocator);

	Model(const std::filesystem::path& modelFile,
		DeviceHandle<B> device, CommandPoolHandle<B> commandPool, QueueHandle<B> queue, AllocatorHandle<B> allocator);

	const auto& getDesc() const { return myDesc; }

	const auto& getVertexBuffer() const { return myVertexBuffer; }
	const auto& getIndexBuffer() const { return myIndexBuffer; }
	
	const auto& getBindings() const { return myBindings; }

private:

	const ModelCreateDesc<B> myDesc = {};

	std::vector<VertexInputBindingDescription<B>> myBindings;

	Buffer<B> myVertexBuffer = {};
	Buffer<B> myIndexBuffer = {};
};
