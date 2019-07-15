#pragma once

#include "aabb.h"
#include "buffer.h"
#include "gfx.h"
#include "vertex.h"

#include <filesystem>
#include <memory>
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

	Model(DeviceHandle<B> device, CommandPoolHandle<B> commandPool, QueueHandle<B> queue, AllocatorHandle<B> allocator,
		ModelCreateDesc<B>&& modelData);

	Model(DeviceHandle<B> device, CommandPoolHandle<B> commandPool, QueueHandle<B> queue, AllocatorHandle<B> allocator,
		const std::filesystem::path& modelFile);

	~Model();

	const auto& getDesc() const { return myDesc; }

	const auto& getVertexBuffer() const { return myVertexBuffer; }
	const auto& getIndexBuffer() const { return myIndexBuffer; }
	
	const auto& getBindings() const { return myBindings; }

private:

	DeviceHandle<B> myDevice = 0;
	AllocatorHandle<B> myAllocator = 0;

	ModelCreateDesc<B> myDesc = {};

	std::vector<VertexInputBindingDescription<B>> myBindings;

	Buffer<B> myVertexBuffer = {};
	Buffer<B> myIndexBuffer = {};
};
